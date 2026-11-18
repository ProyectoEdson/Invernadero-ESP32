import type { Metadata } from "next";
import { AdminPassword } from "../../../components/admin-password";

export const metadata: Metadata = {
  title: "Establecer contraseña | Invernadero ESP32",
  robots: { index: false, follow: false },
};

export default function PasswordPage() {
  return <AdminPassword />;
}
