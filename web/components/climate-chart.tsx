"use client";

import { memo, useMemo } from "react";
import {
  CartesianGrid, Line, LineChart, ReferenceLine, ResponsiveContainer,
  Tooltip, XAxis, YAxis,
} from "recharts";
import { COOLING_OFF_C, COOLING_ON_C, SAMPLE_INTERVAL_MS, type DeviceState } from "../lib/types";

const timeFormat = new Intl.DateTimeFormat("es", { hour: "2-digit", minute: "2-digit", second: "2-digit" });

type ChartPoint = { time: number; temperature: number | null; humidity: number | null };

export const ClimateChart = memo(function ClimateChart({ points }: { points: DeviceState[] }) {
  const data = useMemo(() => {
    const result: ChartPoint[] = [];
    let previousTime: number | null = null;
    for (const point of points) {
      const time = Date.parse(point.received_at) - point.sample_age_ms;
      if (previousTime !== null && time - previousTime > SAMPLE_INTERVAL_MS * 2.5) {
        // A null separator breaks the drawing; it is not a measured sample.
        result.push({ time: previousTime + 1, temperature: null, humidity: null });
      }
      result.push({
        time,
        temperature: point.sensor_status === "ok" ? point.temperature_c : null,
        humidity: point.sensor_status === "ok" ? point.humidity_pct : null,
      });
      previousTime = time;
    }
    return result;
  }, [points]);
  const valid = points.filter((point) => point.sensor_status === "ok").length;

  return (
    <section className="evolution" aria-labelledby="evolution-title">
      <div className="section-heading">
        <div>
          <h2 id="evolution-title">El clima, a su ritmo</h2>
          <p>Evolución de esta visita</p>
        </div>
        <span className="sample-count">{points.length} {points.length === 1 ? "muestra" : "muestras"}</span>
      </div>
      <div className="chart-legend" aria-label="Series y unidades">
        <span><i className="legend-temperature" />Temperatura · °C</span>
        <span><i className="legend-humidity" />Humedad · % HR</span>
      </div>
      {valid === 0 ? (
        <div className="chart-empty">
          <svg viewBox="0 0 160 64" aria-hidden="true"><path d="M8 51H152M18 40L48 30M65 28L91 18M110 21L143 10" /></svg>
          <h3>Aún no hay una curva</h3>
          <p>Las lecturas válidas aparecerán aquí al recibirlas. No se carga un histórico de otras visitas.</p>
        </div>
      ) : (
        <>
          <div className="chart-canvas" role="img" aria-label="Evolución de temperatura en grados Celsius y humedad relativa en porcentaje. Las interrupciones de lectura dejan huecos en las líneas.">
            <ResponsiveContainer width="100%" height="100%" minWidth={0}>
              <LineChart data={data} margin={{ top: 16, right: 0, bottom: 8, left: 0 }} accessibilityLayer>
                <CartesianGrid vertical={false} stroke="#DCE6DA" strokeDasharray="3 6" />
                <XAxis dataKey="time" type="number" domain={["dataMin", "dataMax"]} tickFormatter={(value: number) => timeFormat.format(value)} minTickGap={35} tickLine={false} axisLine={false} tick={{ fill: "#52675A", fontSize: 11 }} />
                <YAxis yAxisId="temperature" domain={[0, 50]} width={36} tickFormatter={(value: number) => `${value}°`} tickLine={false} axisLine={false} tick={{ fill: "#244C3A", fontSize: 11 }} />
                <YAxis yAxisId="humidity" orientation="right" domain={[0, 100]} width={42} tickFormatter={(value: number) => `${value}%`} tickLine={false} axisLine={false} tick={{ fill: "#A25C18", fontSize: 11 }} />
                <ReferenceLine yAxisId="temperature" y={COOLING_ON_C} stroke="#9BBFA7" strokeDasharray="4 5" />
                <ReferenceLine yAxisId="temperature" y={COOLING_OFF_C} stroke="#9BBFA7" strokeDasharray="4 5" />
                <Tooltip filterNull labelFormatter={(value) => timeFormat.format(Number(value))} formatter={(value, name) => [`${value} ${name === "Temperatura" ? "°C" : "% HR"}`, name]} contentStyle={{ background: "#F7FAF5", border: "1px solid #DCE6DA", borderRadius: 12, fontSize: 12 }} />
                <Line yAxisId="temperature" type="linear" dataKey="temperature" name="Temperatura" stroke="#244C3A" strokeWidth={2.5} dot={{ r: valid < 3 ? 4 : 0, strokeWidth: 0, fill: "#244C3A" }} activeDot={{ r: 5 }} connectNulls={false} isAnimationActive={false} />
                <Line yAxisId="humidity" type="linear" dataKey="humidity" name="Humedad" stroke="#A25C18" strokeWidth={1.8} strokeDasharray="5 4" dot={{ r: valid < 3 ? 3 : 0, strokeWidth: 0, fill: "#A25C18" }} activeDot={{ r: 5 }} connectNulls={false} isAnimationActive={false} />
              </LineChart>
            </ResponsiveContainer>
          </div>
          {valid === 1 && <p className="chart-note">Una lectura real. La curva necesita nuevas muestras.</p>}
        </>
      )}
      <p className="chart-footnote">Máximo 1.200 muestras en memoria. Los huecos no se interpolan. Horas de tu navegador.</p>
      <details className="reading-table">
        <summary>Consultar lecturas en tabla</summary>
        {points.length === 0 ? <p>No hay muestras recibidas en esta visita.</p> : (
          <div className="table-scroll" tabIndex={0} role="region" aria-label="Lecturas recibidas, desplazamiento vertical">
            <table>
              <caption>Últimas muestras primero. Hora local del navegador.</caption>
              <thead><tr><th scope="col">Hora</th><th scope="col">°C</th><th scope="col">% HR</th><th scope="col">Sensor</th></tr></thead>
              <tbody>{[...points].reverse().map((point) => (
                <tr key={point.sample_id}>
                  <td>{timeFormat.format(Date.parse(point.received_at) - point.sample_age_ms)}</td>
                  <td>{point.sensor_status === "ok" ? point.temperature_c : "—"}</td>
                  <td>{point.sensor_status === "ok" ? point.humidity_pct : "—"}</td>
                  <td>{point.sensor_status === "ok" ? "Válido" : point.sensor_status === "out_of_range" ? "Fuera de rango" : "Fallo de lectura"}</td>
                </tr>
              ))}</tbody>
            </table>
          </div>
        )}
      </details>
    </section>
  );
});
