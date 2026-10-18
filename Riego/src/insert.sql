-- Ejecutar en Supabase → SQL Editor.
-- Es idempotente: podés correrlo entero las veces que quieras, tanto en una base
-- nueva como en una ya creada. Ojo con crear la vista sin OR REPLACE o las policies
-- sin DROP previo: el editor corre todo en una transacción, así que un error a
-- mitad de camino revierte también lo de abajo.
CREATE TABLE IF NOT EXISTS readings (
    id BIGSERIAL PRIMARY KEY,
    created_at TIMESTAMPTZ DEFAULT NOW(),
    -- cuando llegó al servidor
    recorded_at TIMESTAMPTZ,
    -- cuando se midió realmente (NTP)
    humidity INTEGER NOT NULL,
    watered BOOLEAN NOT NULL DEFAULT FALSE
);

-- Índice también en recorded_at para consultas por fecha real
CREATE INDEX IF NOT EXISTS readings_recorded_at_idx ON readings (recorded_at DESC);

-- Índice para consultas ordenadas por fecha (dashboard)
CREATE INDEX IF NOT EXISTS readings_created_at_idx ON readings (created_at DESC);

-- Vista útil: solo los eventos de riego
CREATE OR REPLACE VIEW waterings AS
SELECT
    id,
    COALESCE(recorded_at, created_at) AS time,
    humidity
FROM
    readings
WHERE
    watered = TRUE
ORDER BY
    time DESC;

-- Política de acceso. El ESP32 escribe DIRECTO a /rest/v1/readings con la anon
-- key (en este proyecto no hay ninguna Edge Function), así que necesita INSERT.
-- Declarar la policy de service_role es innecesario: bypassea RLS por diseño.
ALTER TABLE
    readings ENABLE ROW LEVEL SECURITY;

DROP POLICY IF EXISTS "Solo service role puede leer" ON readings;
CREATE POLICY "Solo service role puede leer" ON readings FOR
SELECT
    TO service_role USING (true);

DROP POLICY IF EXISTS "ESP32 puede insertar" ON readings;
CREATE POLICY "ESP32 puede insertar" ON readings FOR
INSERT
    TO anon WITH CHECK (true);

-- ─── Migración: correr sobre una base YA creada ───────────────────────────────
-- Este bloque es idempotente: podés ejecutarlo las veces que quieras.
-- Requiere PostgreSQL 15+ (Supabase ya lo cumple).

-- 1) RLS no protege a través de una vista. `waterings` es propiedad de postgres y
--    se ejecuta con sus privilegios, y RLS NO se aplica al owner de la tabla. Sin
--    este SET, cualquiera con la anon key (pública por diseño, va en el navegador)
--    puede leer todos los riegos pegándole a /rest/v1/waterings, aunque `readings`
--    le bloquee el select. Con security_invoker la vista respeta el RLS de quien
--    consulta, así que depende de la policy de lectura de abajo (punto 2).
ALTER VIEW waterings SET (security_invoker = true);

-- 2) El dashboard es público y lee con la anon key: hace falta el SELECT explícito.
DROP POLICY IF EXISTS "Dashboard publico puede leer" ON readings;
CREATE POLICY "Dashboard publico puede leer" ON readings FOR
SELECT
    TO anon USING (true);

-- 3) La anon key es pública y puede insertar lo que quiera: acotamos el rango para
--    que nadie ensucie la serie con humedades imposibles.
ALTER TABLE readings DROP CONSTRAINT IF EXISTS readings_humidity_range;
ALTER TABLE readings ADD CONSTRAINT readings_humidity_range CHECK (humidity BETWEEN 0 AND 100);