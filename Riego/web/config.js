// ─── Configuración del dashboard ──────────────────────────────────────────────
//
// Completá estos dos valores con los MISMOS datos que usa el ESP32 (src/secrets.h).
//
// IMPORTANTE: acá va solo la URL del proyecto, SIN la ruta de la tabla.
//   Bien:  https://abcdefgh.supabase.co
//   Mal:   https://abcdefgh.supabase.co/rest/v1/readings
//
// La anon key es pública por diseño: va en el navegador de cualquiera que abra la
// página, y quien la tenga puede leer e insertar en `readings` según lo que
// permitan las políticas RLS. Por eso la tabla tiene la policy de SELECT para
// `anon` y el CHECK de rango en humidity.
//
// NO pongas acá la service_role key: esa saltea RLS y da control total sobre la
// base. Nunca en el navegador.

window.RIEGO_CONFIG = {
  supabaseUrl: "https://uxxikacucnyrzbokgvap.supabase.co",
  supabaseAnonKey: "sb_publishable_tHuUSgKZ0K7T82oBd_eWrA_euvezN1r",
};
