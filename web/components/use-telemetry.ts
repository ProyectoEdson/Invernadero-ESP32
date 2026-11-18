"use client";

import { createClient, type SupabaseClient } from "@supabase/supabase-js";
import { useCallback, useEffect, useRef, useState } from "react";
import { MAX_SESSION_POINTS, PUBLIC_DEVICE_ID, type DeviceState } from "../lib/types";

let browserClient: SupabaseClient | undefined;

function getClient() {
  const url = process.env.NEXT_PUBLIC_SUPABASE_URL;
  const key = process.env.NEXT_PUBLIC_SUPABASE_ANON_KEY;
  if (!url || !key) {
    throw new Error("Faltan NEXT_PUBLIC_SUPABASE_URL o NEXT_PUBLIC_SUPABASE_ANON_KEY en el despliegue.");
  }
  browserClient ??= createClient(url, key);
  return browserClient;
}

type RealtimeState = "connecting" | "connected" | "interrupted";

export function useTelemetry() {
  const [state, setState] = useState<DeviceState | null>(null);
  const [points, setPoints] = useState<DeviceState[]>([]);
  const [loading, setLoading] = useState(true);
  const [queryError, setQueryError] = useState<string | null>(null);
  const [configError, setConfigError] = useState<string | null>(null);
  const [realtime, setRealtime] = useState<RealtimeState>("connecting");
  const [now, setNow] = useState<number | null>(null);
  const refreshRef = useRef<() => void>(() => {});
  const refresh = useCallback(() => refreshRef.current(), []);

  useEffect(() => {
    setNow(Date.now());
    const clock = window.setInterval(() => setNow(Date.now()), 1_000);
    let active = true;
    let request = 0;
    let latest: DeviceState | null = null;
    let client: SupabaseClient;
    try {
      client = getClient();
    } catch (error) {
      setConfigError(error instanceof Error ? error.message : "La configuración pública de Supabase no es válida.");
      setLoading(false);
      return () => window.clearInterval(clock);
    }

    const accept = (next: DeviceState) => {
      if (!active || next.id !== PUBLIC_DEVICE_ID) return;
      // A query started before an event must not replace that newer event.
      if (latest && Date.parse(next.received_at) < Date.parse(latest.received_at)) return;
      latest = next;
      setState(next);
      setPoints((previous) => {
        if (previous.some((point) => point.sample_id === next.sample_id)) return previous;
        return [...previous, next].slice(-MAX_SESSION_POINTS);
      });
    };

    const query = async () => {
      const ownRequest = ++request;
      try {
        const { data, error } = await client
          .from("device_state")
          .select("*")
          .eq("id", PUBLIC_DEVICE_ID)
          .maybeSingle();
        if (!active || ownRequest !== request) return;
        if (error) {
          setQueryError(error.message);
        } else {
          setQueryError(null);
          if (data) accept(data as DeviceState);
          else if (!latest) setState(null);
        }
      } catch {
        if (active && ownRequest === request) setQueryError("No se pudo contactar con Supabase.");
      } finally {
        if (active && ownRequest === request) setLoading(false);
      }
    };

    refreshRef.current = () => void query();
    void query();
    const channel = client
      .channel("public-invernadero")
      .on(
        "postgres_changes",
        { event: "*", schema: "public", table: "device_state", filter: `id=eq.${PUBLIC_DEVICE_ID}` },
        (payload) => {
          if (!active) return;
          if (payload.eventType === "DELETE") {
            latest = null;
            setState(null);
            void query();
          } else {
            accept(payload.new as DeviceState);
          }
        },
      )
      .subscribe((status) => {
        if (!active) return;
        if (status === "SUBSCRIBED") {
          setRealtime("connected");
          // Also closes the gap between the initial SELECT and subscribing.
          void query();
        } else if (status === "CHANNEL_ERROR" || status === "TIMED_OUT" || status === "CLOSED") {
          setRealtime("interrupted");
        }
      });

    const online = () => void query();
    const offline = () => setRealtime("interrupted");
    const visible = () => {
      if (document.visibilityState === "visible") void query();
    };
    window.addEventListener("online", online);
    window.addEventListener("offline", offline);
    document.addEventListener("visibilitychange", visible);
    return () => {
      active = false;
      refreshRef.current = () => {};
      window.clearInterval(clock);
      window.removeEventListener("online", online);
      window.removeEventListener("offline", offline);
      document.removeEventListener("visibilitychange", visible);
      void client.removeChannel(channel);
    };
  }, []);

  return { state, points, loading, queryError, configError, realtime, now, refresh };
}
