-- Fase 4: mandos privados, ligados al arranque y confirmados por el ESP32.
begin;

alter table public.device_state
  drop constraint device_state_gpio_levels_check,
  drop constraint device_state_operating_mode_check,
  add column phase4_control_ready boolean not null default false,
  add constraint device_state_gpio_levels_check check (gpio_levels in (
    '{"peltier":0,"pump":0,"fans":0}'::jsonb,
    '{"peltier":0,"pump":1,"fans":0}'::jsonb,
    '{"peltier":0,"pump":0,"fans":1}'::jsonb,
    '{"peltier":0,"pump":1,"fans":1}'::jsonb
  )),
  add constraint device_state_operating_mode_check
    check (operating_mode in ('monitoring', 'manual', 'automatic')),
  add constraint device_state_phase4_mode_check
    check (not phase4_control_ready or operating_mode in ('manual', 'automatic'));

-- La frescura y el modo se comprueban contra el cursor privado del dispositivo,
-- no contra una identidad de arranque publicada ni contra datos del navegador.
alter table private.ingest_cursor
  add column received_at timestamptz,
  add column sample_age_ms integer,
  add column operating_mode text not null default 'monitoring',
  add column phase4_control_ready boolean not null default false;

create table private.device_commands (
  id uuid primary key default pg_catalog.gen_random_uuid(),
  sequence bigint generated always as identity unique not null
    check (sequence between 1 and 9007199254740991),
  device_id text not null check (device_id ~ '^inv-[0-9a-f]{12}$'),
  target_boot_id text not null check (target_boot_id ~ '^[0-9a-f]{32}$'),
  kind text not null check (kind in ('set_mode', 'pump_pulse', 'set_fans')),
  value jsonb,
  duration_ms integer,
  status text not null default 'pending' check (status in ('pending', 'applied', 'rejected', 'expired')),
  reason text check (pg_catalog.char_length(reason) <= 160),
  created_at timestamptz not null,
  expires_at timestamptz not null,
  completed_at timestamptz,
  constraint device_commands_valid_action check (
    (kind = 'set_mode' and value is not null and value in ('"manual"'::jsonb, '"automatic"'::jsonb)
      and duration_ms is null)
    or (kind = 'set_fans' and value is not null and value in ('true'::jsonb, 'false'::jsonb)
      and duration_ms is null)
    or (kind = 'pump_pulse' and value is null and duration_ms is not null and duration_ms between 1 and 3000)
  ),
  constraint device_commands_valid_expiry check (expires_at = created_at + interval '15 seconds'),
  constraint device_commands_valid_completion check (
    (status = 'pending' and completed_at is null and reason is null)
    or (status <> 'pending' and completed_at is not null)
  )
);
create unique index device_commands_one_pending on private.device_commands (device_id)
  where status = 'pending';
create index device_commands_recent on private.device_commands (device_id, sequence desc);
revoke all on table private.device_commands from public, anon, authenticated;
revoke all on sequence private.device_commands_sequence_seq from public, anon, authenticated;

-- Proyección explícita: ni device_id ni target_boot_id llegan a la API admin.
create function private.command_view(command private.device_commands)
returns jsonb
language sql
stable
set search_path = ''
as $$
  select pg_catalog.jsonb_build_object(
    'id', command.id, 'sequence', command.sequence, 'kind', command.kind,
    'value', command.value, 'duration_ms', command.duration_ms,
    'status', command.status, 'reason', command.reason,
    'created_at', command.created_at, 'expires_at', command.expires_at,
    'completed_at', command.completed_at
  );
$$;
revoke all on function private.command_view(private.device_commands) from public, anon, authenticated;

-- Cada llamador mantiene antes el mismo advisory lock por dispositivo.
create function private.expire_device_commands(p_device_id text, p_now timestamptz)
returns void
language sql
set search_path = ''
as $$
  update private.device_commands set status = 'expired',
    reason = 'Orden vencida sin confirmación.', completed_at = p_now
  where device_id = p_device_id and status = 'pending' and expires_at <= p_now;
$$;
revoke all on function private.expire_device_commands(text, timestamptz) from public, anon, authenticated;

create function public.admin_device_state(p_device_id text)
returns jsonb
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_state jsonb;
  v_commands jsonb;
begin
  if p_device_id is null or p_device_id !~ '^inv-[0-9a-f]{12}$' then
    raise exception 'Invalid device' using errcode = '22023';
  end if;
  perform pg_catalog.pg_advisory_xact_lock(pg_catalog.hashtextextended(p_device_id, 0));
  perform private.expire_device_commands(p_device_id, pg_catalog.clock_timestamp());
  select pg_catalog.to_jsonb(state) into v_state
    from public.device_state state where id = 'invernadero';
  select coalesce(pg_catalog.jsonb_agg(recent.body order by recent.sequence desc), '[]'::jsonb)
    into v_commands
    from (
      select private.command_view(command) as body, command.sequence
      from private.device_commands command where command.device_id = p_device_id
      order by command.sequence desc limit 20
    ) recent;
  return pg_catalog.jsonb_build_object('state', v_state, 'commands', v_commands);
end;
$$;
revoke all on function public.admin_device_state(text) from public, anon, authenticated;
grant execute on function public.admin_device_state(text) to service_role;

create function public.create_device_command(p_device_id text, payload jsonb)
returns jsonb
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_cursor private.ingest_cursor%rowtype;
  v_command private.device_commands%rowtype;
  v_now timestamptz;
  v_kind text := payload ->> 'kind';
  v_value jsonb;
  v_duration integer;
begin
  if p_device_id is null or p_device_id !~ '^inv-[0-9a-f]{12}$'
      or pg_catalog.jsonb_typeof(payload) is distinct from 'object' then
    raise exception 'Invalid command' using errcode = '22023';
  end if;
  if v_kind = 'set_mode' and payload -> 'value' in ('"manual"'::jsonb, '"automatic"'::jsonb)
      and payload - 'kind' - 'value' = '{}'::jsonb then
    v_value := payload -> 'value';
  elsif v_kind = 'set_fans' and payload -> 'value' in ('true'::jsonb, 'false'::jsonb)
      and payload - 'kind' - 'value' = '{}'::jsonb then
    v_value := payload -> 'value';
  elsif v_kind = 'pump_pulse' and pg_catalog.jsonb_typeof(payload -> 'duration_ms') = 'number'
      and (payload ->> 'duration_ms') ~ '^[0-9]{1,4}$'
      and (payload ->> 'duration_ms')::integer between 1 and 3000
      and payload - 'kind' - 'duration_ms' = '{}'::jsonb then
    v_duration := (payload ->> 'duration_ms')::integer;
  else
    raise exception 'Invalid command' using errcode = '22023';
  end if;

  perform pg_catalog.pg_advisory_xact_lock(pg_catalog.hashtextextended(p_device_id, 0));
  v_now := pg_catalog.clock_timestamp();
  perform private.expire_device_commands(p_device_id, v_now);
  select * into v_cursor from private.ingest_cursor where device_id = p_device_id;
  if not found or not v_cursor.phase4_control_ready or v_cursor.received_at is null
      or v_cursor.sample_age_ms is null
      or v_cursor.received_at - v_cursor.sample_age_ms * interval '1 millisecond' <= v_now - interval '15 seconds' then
    raise exception 'device_not_ready' using errcode = 'P0001';
  end if;
  if exists (select 1 from private.device_commands where device_id = p_device_id and status = 'pending') then
    raise exception 'command_pending' using errcode = 'P0001';
  end if;
  if v_kind = 'pump_pulse' and v_cursor.operating_mode <> 'manual' then
    raise exception 'pump_requires_manual' using errcode = 'P0001';
  end if;
  insert into private.device_commands (
    device_id, target_boot_id, kind, value, duration_ms, created_at, expires_at
  ) values (
    p_device_id, v_cursor.boot_id, v_kind, v_value, v_duration, v_now, v_now + interval '15 seconds'
  ) returning * into v_command;
  return private.command_view(v_command);
end;
$$;
revoke all on function public.create_device_command(text, jsonb) from public, anon, authenticated;
grant execute on function public.create_device_command(text, jsonb) to service_role;

-- Corte limpio: el firmware fase 3 no es compatible con el protocolo de mandos.
drop function public.ingest_telemetry(jsonb);
create function public.ingest_telemetry_v4(payload jsonb)
returns jsonb
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_device text := payload ->> 'device_id';
  v_boot text := payload ->> 'boot_id';
  v_sequence bigint := (payload ->> 'sequence')::bigint;
  v_uptime bigint := (payload ->> 'uptime_ms')::bigint;
  v_sent bigint := (payload ->> 'sent_uptime_ms')::bigint;
  v_age integer;
  v_cursor private.ingest_cursor%rowtype;
  v_now timestamptz;
  v_ack jsonb := payload -> 'ack';
  v_command jsonb;
begin
  if payload ->> 'type' is distinct from 'sample' or v_device is null or v_boot is null
      or v_sequence is null or v_uptime is null or v_sent is null
      or v_device !~ '^inv-[0-9a-f]{12}$' or v_boot !~ '^[0-9a-f]{32}$'
      or v_sequence < 1 or v_sequence > 4294967295
      or v_uptime < 0 or v_sent < v_uptime or v_sent > 9007199254740991 or v_sent - v_uptime > 15000
      or payload -> 'wifi_connected' not in ('true'::jsonb, 'false'::jsonb)
      or not (payload ? 'wifi_connected')
      or payload -> 'memory_config_ok' not in ('true'::jsonb, 'false'::jsonb)
      or not (payload ? 'memory_config_ok')
      or payload -> 'peltier_locked' is distinct from 'true'::jsonb
      or payload -> 'outputs_safe' is distinct from 'true'::jsonb
      or payload -> 'phase4_control_ready' is distinct from 'true'::jsonb
      or payload ->> 'operating_mode' is null or payload ->> 'operating_mode' not in ('manual', 'automatic')
      or v_ack is null then
    raise exception 'Invalid telemetry' using errcode = '22023';
  end if;
  if v_ack <> 'null'::jsonb and (
      pg_catalog.jsonb_typeof(v_ack) is distinct from 'object'
      or coalesce(v_ack ->> 'id', '') !~* '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$'
      or coalesce(v_ack ->> 'status', '') not in ('applied', 'rejected')
      or not (v_ack ? 'reason')
      or pg_catalog.jsonb_typeof(v_ack -> 'reason') not in ('null', 'string')
      or pg_catalog.char_length(v_ack ->> 'reason') > 160
    ) then
    raise exception 'Invalid acknowledgement' using errcode = '22023';
  end if;
  v_age := (v_sent - v_uptime)::integer;
  perform pg_catalog.pg_advisory_xact_lock(pg_catalog.hashtextextended(v_device, 0));
  v_now := pg_catalog.clock_timestamp();
  perform private.expire_device_commands(v_device, v_now);
  select * into v_cursor from private.ingest_cursor where device_id = v_device;
  if found then
    if v_cursor.boot_id = v_boot then
      if v_sequence <= v_cursor.sequence or v_uptime < v_cursor.uptime_ms then
        return pg_catalog.jsonb_build_object('accepted', false, 'command', null);
      end if;
    elsif exists (select 1 from private.known_boots where device_id = v_device and boot_id = v_boot) then
      return pg_catalog.jsonb_build_object('accepted', false, 'command', null);
    end if;
  end if;

  -- Un nuevo arranque no hereda órdenes, ni siquiera durante sus 15 s de validez.
  update private.device_commands set status = 'rejected',
    reason = 'Dispositivo reiniciado antes de confirmar la orden.', completed_at = v_now
  where device_id = v_device and status = 'pending' and target_boot_id <> v_boot;

  -- Repetir un ack nunca cambia un resultado terminal ni una orden de otro boot.
  if v_ack <> 'null'::jsonb then
    update private.device_commands set status = v_ack ->> 'status',
      reason = v_ack ->> 'reason', completed_at = v_now
    where id = (v_ack ->> 'id')::uuid and device_id = v_device
      and target_boot_id = v_boot and status = 'pending' and expires_at > v_now;
  end if;

  insert into private.known_boots (device_id, boot_id) values (v_device, v_boot)
    on conflict do nothing;
  insert into private.ingest_cursor (
    device_id, boot_id, sequence, uptime_ms, received_at, sample_age_ms, operating_mode, phase4_control_ready
  ) values (
    v_device, v_boot, v_sequence, v_uptime, v_now, v_age, payload ->> 'operating_mode', true
  ) on conflict (device_id) do update set
    boot_id = excluded.boot_id, sequence = excluded.sequence, uptime_ms = excluded.uptime_ms,
    received_at = excluded.received_at, sample_age_ms = excluded.sample_age_ms,
    operating_mode = excluded.operating_mode, phase4_control_ready = excluded.phase4_control_ready;

  insert into public.device_state (
    id, sample_id, received_at, sample_age_ms, temperature_c, humidity_pct,
    sensor_status, wifi_connected, memory_config_ok, peltier_locked, gpio_levels,
    outputs_safe, operating_mode, phase4_control_ready
  ) values (
    'invernadero', pg_catalog.gen_random_uuid(), v_now, v_age,
    (payload ->> 'temperature_c')::double precision, (payload ->> 'humidity_pct')::double precision,
    payload ->> 'sensor_status', (payload ->> 'wifi_connected')::boolean,
    (payload ->> 'memory_config_ok')::boolean, (payload ->> 'peltier_locked')::boolean,
    payload -> 'gpio_levels', (payload ->> 'outputs_safe')::boolean, payload ->> 'operating_mode', true
  ) on conflict (id) do update set
    sample_id = excluded.sample_id, received_at = excluded.received_at,
    sample_age_ms = excluded.sample_age_ms, temperature_c = excluded.temperature_c,
    humidity_pct = excluded.humidity_pct, sensor_status = excluded.sensor_status,
    wifi_connected = excluded.wifi_connected, memory_config_ok = excluded.memory_config_ok,
    peltier_locked = excluded.peltier_locked, gpio_levels = excluded.gpio_levels,
    outputs_safe = excluded.outputs_safe, operating_mode = excluded.operating_mode,
    phase4_control_ready = excluded.phase4_control_ready;

  -- Calcular TTL después de las escrituras y nunca reenviar desde un replay.
  v_now := pg_catalog.clock_timestamp();
  perform private.expire_device_commands(v_device, v_now);
  select pg_catalog.jsonb_build_object(
    'id', command.id, 'sequence', command.sequence, 'kind', command.kind,
    'value', command.value, 'duration_ms', command.duration_ms,
    'target_boot_id', command.target_boot_id,
    'expires_in_ms', least(15000,
      pg_catalog.floor(extract(epoch from (command.expires_at - v_now)) * 1000)::integer)
  ) into v_command from private.device_commands command
  where command.device_id = v_device and command.target_boot_id = v_boot
    and command.status = 'pending' and command.expires_at >= v_now + interval '1 millisecond';
  return pg_catalog.jsonb_build_object('accepted', true, 'command', v_command);
end;
$$;
revoke all on function public.ingest_telemetry_v4(jsonb) from public, anon, authenticated;
grant execute on function public.ingest_telemetry_v4(jsonb) to service_role;

-- Conserva exactamente los permisos públicos de solo lectura y la publicación
-- Realtime existentes; no se publica ni se concede acceso directo a órdenes.
commit;
