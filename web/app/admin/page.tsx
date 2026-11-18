import type { Metadata } from "next";
import { AdminPanel } from "../../components/admin-panel";

export const metadata: Metadata = {
  title: "Administración | Invernadero ESP32",
  robots: { index: false, follow: false },
};

export default function AdminPage() {
  return <AdminPanel />;
}
