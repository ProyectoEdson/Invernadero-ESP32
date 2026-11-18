import type { ReactNode } from "react";

export function AdminShell({ children, action }: { children: ReactNode; action?: ReactNode }) {
  return (
    <div className="site-shell admin-shell">
      <a className="skip-link" href="#administracion">Saltar al acceso privado</a>
      <header className="site-header admin-header">
        <a className="identity" href="/" aria-label="Invernadero ESP32, lecturas públicas">
          <svg className="brand-mark" viewBox="0 0 40 44" aria-hidden="true"><path d="M5 38V17L20 5L35 17V38H5ZM20 5V38M5 17H35M20 30C11 30 10 24 10 24C17 23 20 27 20 30ZM20 25C20 20 25 18 29 19C29 24 25 27 20 27" /></svg>
          <span>Invernadero <span className="identity-model">ESP32</span></span>
        </a>
        <nav className="admin-navigation" aria-label="Administración"><a href="/">Ver clima</a>{action}</nav>
      </header>
      <main id="administracion">{children}</main>
      <footer className="site-footer"><span>Acceso privado · Invernadero ESP32</span><span>Las órdenes requieren confirmación del dispositivo.</span></footer>
    </div>
  );
}
