-- Fase 3: una fila pública, sin historial ilimitado ni mandos de actuadores.
begin;

create schema if not exists private;
revoke all on schema private from public, anon, authenticated;

create table public.device_state (
  id text primary key check (id = 'invernadero'),
  sample_id uuid not null,
  received_at timestamptz not null,
  sample_age_ms integer not null check (sample_age_ms between 0 and 15000),
  temperature_c double precision,
  humidity_pct double precision,
  sensor_status text not null check (sensor_status in ('ok', 'read_error', 'out_of_range')),
  wifi_connected boolean not null,
  memory_config_ok boolean not null,
  peltier_locked boolean not null check (peltier_locked),
  gpio_levels jsonb not null check (gpio_levels = '{"peltier":0,"pump":0,"fans":0}'::jsonb),
  outputs_safe boolean not null check (outputs_safe),
  operating_mode text not null default 'monitoring' check (operating_mode = 'monitoring'),
  constraint valid_sensor_values check (
    (sensor_status = 'ok' and temperature_c is not null and humidity_pct is not null
      and temperature_c between 0 and 50 and humidity_pct between 0 and 100)
    or (sensor_status <> 'ok' and temperature_c is null and humidity_pct is null)
  )
);
alter table public.device_state enable row level security;
revoke all on table public.device_state from public, anon, authenticated;
grant select on table public.device_state to anon, authenticated;
create policy "Public may read telemetry" on public.device_state
  for select to anon, authenticated using (true);

create table private.ingest_cursor (
  device_id text primary key,
  boot_id text not null,
  sequence bigint not null,
  uptime_ms bigint not null
);
create table private.known_boots (
  device_id text not null,
  boot_id text not null,
  primary key (device_id, boot_id)
);
revoke all on table private.ingest_cursor, private.known_boots from public, anon, authenticated;

-- Solo el backend privilegiado puede invocar esta función. La transacción y
-- el lock evitan que un reintento o peticiones concurrentes hagan retroceder datos.
create function public.ingest_telemetry(payload jsonb)
returns boolean
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_device text := payload ->> 'device_id';
  v_boot text := payload ->> 'boot_id';
  v_sequence bigint := (payload ->> 'sequence')::bigint;
  v_uptime bigint := (payload ->> 'uptime_ms')::bigint;
  v_age integer := ((payload ->> 'sent_uptime_ms')::bigint - v_uptime)::integer;
  v_cursor private.ingest_cursor%rowtype;
  v_received timestamptz;
begin
  if v_device is null or v_boot is null or v_sequence is null or v_uptime is null
      or v_age is null or v_device !~ '^inv-[0-9a-f]{12}$'
      or v_boot !~ '^[0-9a-f]{32}$' or v_sequence < 1 or v_sequence > 4294967295
      or v_uptime < 0 or v_age < 0 or v_age > 15000 then
    raise exception 'Invalid telemetry' using errcode = '22023';
  end if;
  perform pg_catalog.pg_advisory_xact_lock(pg_catalog.hashtextextended(v_device, 0));
  select * into v_cursor from private.ingest_cursor where device_id = v_device;
  if found then
    if v_cursor.boot_id = v_boot then
      if v_sequence <= v_cursor.sequence or v_uptime < v_cursor.uptime_ms then
        return false;
      end if;
    elsif exists (select 1 from private.known_boots where device_id = v_device and boot_id = v_boot) then
      return false;
    end if;
  end if;

  insert into private.known_boots (device_id, boot_id) values (v_device, v_boot)
    on conflict do nothing;
  insert into private.ingest_cursor (device_id, boot_id, sequence, uptime_ms)
    values (v_device, v_boot, v_sequence, v_uptime)
    on conflict (device_id) do update set
      boot_id = excluded.boot_id, sequence = excluded.sequence, uptime_ms = excluded.uptime_ms;

  v_received := pg_catalog.clock_timestamp();
  insert into public.device_state (
    id, sample_id, received_at, sample_age_ms, temperature_c, humidity_pct,
    sensor_status, wifi_connected, memory_config_ok, peltier_locked, gpio_levels, outputs_safe
  ) values (
    'invernadero', pg_catalog.gen_random_uuid(), v_received, v_age,
    (payload ->> 'temperature_c')::double precision, (payload ->> 'humidity_pct')::double precision,
    payload ->> 'sensor_status', (payload ->> 'wifi_connected')::boolean,
    (payload ->> 'memory_config_ok')::boolean, (payload ->> 'peltier_locked')::boolean,
    payload -> 'gpio_levels', (payload ->> 'outputs_safe')::boolean
  ) on conflict (id) do update set
    sample_id = excluded.sample_id, received_at = excluded.received_at,
    sample_age_ms = excluded.sample_age_ms, temperature_c = excluded.temperature_c,
    humidity_pct = excluded.humidity_pct, sensor_status = excluded.sensor_status,
    wifi_connected = excluded.wifi_connected, memory_config_ok = excluded.memory_config_ok,
    peltier_locked = excluded.peltier_locked, gpio_levels = excluded.gpio_levels,
    outputs_safe = excluded.outputs_safe;
  return true;
end;
$$;
revoke all on function public.ingest_telemetry(jsonb) from public, anon, authenticated;
grant execute on function public.ingest_telemetry(jsonb) to service_role;

do $$
begin
  if not exists (
    select 1 from pg_catalog.pg_publication_tables
    where pubname = 'supabase_realtime' and schemaname = 'public' and tablename = 'device_state'
  ) then
    alter publication supabase_realtime add table public.device_state;
  end if;
end;
$$;

commit;
