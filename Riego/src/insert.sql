-- Ejecutar en Supabase → SQL Editor
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
CREATE VIEW waterings AS
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

-- Política de acceso: solo la service role puede insertar (el ESP32 usa anon key
-- pero la Edge Function usa service role internamente, así que esto es correcto)
ALTER TABLE
    readings ENABLE ROW LEVEL SECURITY;

CREATE POLICY "Solo service role puede leer" ON readings FOR
SELECT
    TO service_role USING (true);

CREATE POLICY "ESP32 puede insertar" ON readings FOR
INSERT
    TO anon WITH CHECK (true);