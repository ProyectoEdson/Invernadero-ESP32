ALTER TABLE readings
    ADD COLUMN illuminance_lux DOUBLE NULL AFTER humidity_pct,
    MODIFY COLUMN fans_gpio BOOLEAN NULL,
    ADD COLUMN leds_gpio BOOLEAN NULL AFTER fans_gpio,
    MODIFY COLUMN operating_mode ENUM('manual', 'automatic', 'monitoring') NOT NULL,
    ADD CONSTRAINT readings_illuminance_range CHECK (illuminance_lux IS NULL OR illuminance_lux BETWEEN 0 AND 54612.5),
    ADD CONSTRAINT readings_mode_outputs CHECK (
        (operating_mode = 'monitoring' AND pump_gpio = 0 AND fans_gpio IS NULL
            AND leds_gpio IS NOT NULL AND leds_gpio = 0)
        OR (operating_mode IN ('manual', 'automatic') AND fans_gpio IS NOT NULL
            AND leds_gpio IS NULL AND illuminance_lux IS NULL)
    );
