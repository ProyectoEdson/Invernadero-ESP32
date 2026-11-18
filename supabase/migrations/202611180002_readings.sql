-- Historial visible: una fila cada 10 s como máximo y 30 días de retención.
-- Se alimenta con un trigger sobre device_state: el RPC de ingreso no cambia.
begin;

create table public.readings (
  id bigint generated always as identity primary key,
  recorded_at timestamptz not null,
  temperature_c double precision,
  humidity_pct double precision,
  sensor_status text not null check (sensor_status in ('ok', 'read_error', 'out_of_range')),
  operating_mode text not null check (operating_mode in ('manual', 'automatic', 'monitoring')),
  gpio_levels jsonb not null,
  sample_id uuid not null unique,
  constraint readings_values check (
    (sensor_status = 'ok' and temperature_c is not null and humidity_pct is not null)
    or (sensor_status <> 'ok' and temperature_c is null and humidity_pct is null)
  )
);

create index readings_recorded_at_desc on public.readings (recorded_at desc);

alter table public.readings enable row level security;
revoke all on table public.readings from public, anon, authenticated;
grant select on table public.readings to anon, authenticated;
create policy "Public may read reading history" on public.readings
  for select to anon, authenticated using (true);

create function private.capture_reading()
returns trigger
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_recorded timestamptz;
begin
  -- 10 s de separación mínima; el índice desc hace la comprobación barata.
  select max(recorded_at) into v_recorded from public.readings;
  if v_recorded is null or new.received_at - v_recorded >= interval '10 seconds' then
    insert into public.readings (
      recorded_at, temperature_c, humidity_pct, sensor_status, operating_mode,
      gpio_levels, sample_id
    ) values (
      new.received_at - pg_catalog.make_interval(secs => new.sample_age_ms / 1000.0),
      new.temperature_c, new.humidity_pct, new.sensor_status, new.operating_mode,
      new.gpio_levels, new.sample_id
    ) on conflict (sample_id) do nothing;
  end if;
  -- Poda perezosa una vez por minuto, sin jobs programados.
  if extract(epoch from new.received_at)::bigint % 60 = 0 then
    delete from public.readings where recorded_at < new.received_at - interval '30 days';
  end if;
  return new;
end;
$$;

revoke all on function private.capture_reading() from public, anon, authenticated;

create trigger device_state_reading
after insert or update on public.device_state
for each row execute function private.capture_reading();

commit;
