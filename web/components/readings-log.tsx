"use client";

import { useCallback, useEffect, useState } from "react";
import { browserAuth } from "../lib/supabase-browser";

const PAGE = 50;
const timeFormat = new Intl.DateTimeFormat("es", {
  day: "2-digit", month: "short", hour: "2-digit", minute: "2-digit", second: "2-digit",
});

type Reading = {
  id: number;
  recorded_at: string;
  temperature_c: number | null;
  humidity_pct: number | null;
  sensor_status: "ok" | "read_error" | "out_of_range";
  operating_mode: "manual" | "automatic" | "monitoring";
};

const COLUMNS = "id, recorded_at, temperature_c, humidity_pct, sensor_status, operating_mode";

export function ReadingsLog() {
  const [rows, setRows] = useState<Reading[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [exhausted, setExhausted] = useState(false);

  const load = useCallback(async (before?: string) => {
    setLoading(true);
    try {
      let query = browserAuth().from("readings").select(COLUMNS).order("recorded_at", { ascending: false }).limit(PAGE);
      if (before) query = query.lt("recorded_at", before);
      const { data, error: queryError } = await query;
      if (queryError) {
        setError("No se pudo leer el registro guardado.");
        return;
      }
      const page = (data ?? []) as Reading[];
      setError(null);
      setRows((previous) => (before ? [...previous, ...page] : page));
      setExhausted(page.length < PAGE);
    } catch {
      setError("No se pudo contactar con la base de datos.");
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => { void load(); }, [load]);

  return (
    <section className="system-section" aria-labelledby="log-title">
      <div className="system-heading">
        <h2 id="log-title">Registro guardado</h2>
        <p>Una fila cada 10 s · 30 días de retención · base de datos</p>
      </div>
      {error && <p role="alert" className="log-note">{error}</p>}
      {!error && rows.length === 0 && !loading && (
        <p className="log-note">Todavía no hay lecturas guardadas. Se registran mientras el ESP32 esté enviando datos.</p>
      )}
      {rows.length > 0 && (
        <>
          <div className="table-scroll">
            <table>
              <caption>Últimas {rows.length} filas guardadas, la más reciente primero.</caption>
              <thead>
                <tr><th scope="col">Hora</th><th scope="col">°C</th><th scope="col">% HR</th><th scope="col">Sensor</th><th scope="col">Modo</th></tr>
              </thead>
              <tbody>
                {rows.map((row) => (
                  <tr key={row.id}>
                    <td>{timeFormat.format(Date.parse(row.recorded_at))}</td>
                    <td>{row.temperature_c ?? "—"}</td>
                    <td>{row.humidity_pct ?? "—"}</td>
                    <td>{row.sensor_status === "ok" ? "Válido" : row.sensor_status === "out_of_range" ? "Fuera de rango" : "Fallo"}</td>
                    <td>{row.operating_mode === "manual" ? "Manual" : row.operating_mode === "automatic" ? "Automático" : "Monitorización"}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
          <div className="log-actions">
            {!exhausted && (
              <button type="button" className="admin-quiet-button" disabled={loading} onClick={() => void load(rows[rows.length - 1]?.recorded_at)}>
                {loading ? "Cargando…" : "Ver más antiguas"}
              </button>
            )}
            <span className="log-note">{exhausted ? "Fin del registro disponible." : ""}</span>
          </div>
        </>
      )}
    </section>
  );
}
