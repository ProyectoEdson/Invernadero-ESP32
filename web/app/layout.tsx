import type { Metadata } from "next";
import "@fontsource-variable/manrope";
import "./globals.css";

export const metadata: Metadata = {
  title: "Invernadero ESP32 | Clima en directo",
  description:
    "Temperatura, humedad ambiental y estado reportado del invernadero ESP32. Monitorización pública con lecturas reales.",
};

export default function RootLayout({ children }: Readonly<{ children: React.ReactNode }>) {
  return (
    <html lang="es">
      <body>{children}</body>
    </html>
  );
}
