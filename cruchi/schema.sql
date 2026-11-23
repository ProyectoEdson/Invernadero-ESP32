CREATE TABLE readings (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    boot_id BINARY(16) NOT NULL,
    sequence_no INT UNSIGNED NOT NULL,
    uptime_ms BIGINT UNSIGNED NOT NULL,
    received_at DATETIME(3) NOT NULL,
    sample_age_ms SMALLINT UNSIGNED NOT NULL,
    temperature_c DOUBLE NULL,
    humidity_pct DOUBLE NULL,
    sensor_status ENUM('ok', 'read_error', 'out_of_range') NOT NULL,
    wifi_connected BOOLEAN NOT NULL,
    memory_config_ok BOOLEAN NOT NULL,
    pump_gpio BOOLEAN NOT NULL,
    fans_gpio BOOLEAN NOT NULL,
    operating_mode ENUM('manual', 'automatic') NOT NULL,
    PRIMARY KEY (id),
    UNIQUE KEY readings_boot_sequence (boot_id, sequence_no),
    KEY readings_received_at (received_at),
    CHECK (sample_age_ms <= 15000),
    CHECK (
        (sensor_status = 'ok' AND temperature_c IS NOT NULL AND humidity_pct IS NOT NULL
            AND temperature_c BETWEEN 0 AND 50 AND humidity_pct BETWEEN 0 AND 100)
        OR (sensor_status IN ('read_error', 'out_of_range') AND temperature_c IS NULL AND humidity_pct IS NULL)
    )
) ENGINE=InnoDB;

-- El cursor identifica la última lectura sin duplicar los datos del sensor.
-- La fila inicial también permite serializar dos primeras escrituras concurrentes.
CREATE TABLE device_state (
    id VARCHAR(16) NOT NULL,
    reading_id BIGINT UNSIGNED NULL,
    PRIMARY KEY (id),
    FOREIGN KEY (reading_id) REFERENCES readings (id),
    CHECK (id = 'invernadero')
) ENGINE=InnoDB;

INSERT INTO device_state (id, reading_id) VALUES ('invernadero', NULL);
