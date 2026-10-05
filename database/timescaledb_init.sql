-- ==============================================================================
-- OPEN-SOURCE CROWDSOURCED WEATHER ECOSYSTEM: DATABASE INITIALIZATION SCRIPT
-- Engine: PostgreSQL 15+ with TimescaleDB & PostGIS Extensions
-- Partitioning: 7-day chunks, Continuous Aggregates (15m, 1h, 1d), Compression
-- ==============================================================================

-- 1. Enable Core Extensions
CREATE EXTENSION IF NOT EXISTS timescaledb CASCADE;
CREATE EXTENSION IF NOT EXISTS postgis CASCADE;
CREATE EXTENSION IF NOT EXISTS pgcrypto CASCADE;

-- 2. Enumerated Types for Ecosystem
DO $$ BEGIN
    CREATE TYPE topology_type_enum AS ENUM (
        'WIFI6_URBAN',    -- Topology 1: Standalone ESP32-C6 with TWT
        'SUBGHZ_DECK',   -- Topology 2: Node A (C3+LoRa) -> Node B (S3 Hub)
        'OFFGRID_MESH'   -- Topology 3: Autonomous ESP32-C3 LoRa Mesh -> S3 DTN
    );
EXCEPTION
    WHEN duplicate_object THEN null;
END $$;

-- 3. Station Metadata Table (Zero-Trust Identity, Spatial Index, SQI Track)
CREATE TABLE IF NOT EXISTS station_metadata (
    station_id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    hardware_uid VARCHAR(64) UNIQUE NOT NULL,
    hardware_model VARCHAR(32) NOT NULL,
    topology_type topology_type_enum NOT NULL,
    
    -- Spatial Coordinates (WGS84 EPSG:4326)
    location GEOMETRY(Point, 4326) NOT NULL,
    altitude_m NUMERIC(6, 2) NOT NULL DEFAULT 0.00,
    
    -- Cryptographic Zero-Trust Provisioning
    public_key_pem TEXT NOT NULL,
    auth_token_hash VARCHAR(128) NOT NULL,
    
    -- Quality Control & Demotion System
    sqi_score NUMERIC(5, 2) NOT NULL DEFAULT 100.00 CHECK (sqi_score >= 0.00 AND sqi_score <= 100.00),
    is_public BOOLEAN GENERATED ALWAYS AS (sqi_score >= 60.00) STORED,
    last_clean_timestamp TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    total_anomalies_flagged INTEGER NOT NULL DEFAULT 0,
    
    -- Lifecycle
    firmware_version VARCHAR(16) NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- PostGIS Spatial Index for Inverse Distance Weighting (IDW) Queries
CREATE INDEX IF NOT EXISTS idx_station_metadata_location 
    ON station_metadata USING GIST (location);

-- Index for public API query filtration
CREATE INDEX IF NOT EXISTS idx_station_metadata_public_sqi 
    ON station_metadata (is_public, sqi_score DESC);

-- 4. Raw Telemetry Hypertable
CREATE TABLE IF NOT EXISTS telemetry (
    time TIMESTAMPTZ NOT NULL,
    station_id UUID NOT NULL REFERENCES station_metadata(station_id) ON DELETE CASCADE,
    
    -- Atmospheric Physical Telemetry
    temperature_c NUMERIC(5, 2),            -- Range: -80.00 to +70.00
    humidity_pct NUMERIC(5, 2),             -- Range: 0.00 to 100.00
    pressure_hpa NUMERIC(6, 2),             -- Range: 800.00 to 1100.00
    air_quality_ppm NUMERIC(7, 2),          -- MQ-135 reading (Null if on battery)
    
    -- Electrical & RF Diagnostics
    battery_mv SMALLINT NOT NULL,           -- Range: 2500 to 3650 mV
    is_mains_powered BOOLEAN NOT NULL DEFAULT FALSE,
    rssi_dbm SMALLINT,                      -- RF Signal strength
    snr_db NUMERIC(4, 1),                   -- LoRa SNR (null for Wi-Fi 6)
    mesh_hop_count SMALLINT DEFAULT 0,
    
    -- Quality Control Bitmask
    -- Bit 0: HARDWARE_LIMIT_FAIL (0x01)
    -- Bit 1: TEMPORAL_DELTA_FAIL (0x02)
    -- Bit 2: SPATIAL_ANOMALY     (0x04)
    -- Bit 3: SIGNATURE_INVALID   (0x08)
    qc_flags INTEGER NOT NULL DEFAULT 0,
    
    -- Signature Verification
    signature BYTEA,
    
    CONSTRAINT pk_telemetry PRIMARY KEY (time, station_id)
);

-- Convert to 7-day chunk TimescaleDB Hypertable
SELECT create_hypertable(
    'telemetry', 
    'time', 
    chunk_time_interval => INTERVAL '7 days',
    if_not_exists => TRUE
);

-- Compound Index for fast point-lookup and range scans
CREATE INDEX IF NOT EXISTS idx_telemetry_station_time 
    ON telemetry (station_id, time DESC);

-- 5. Native TimescaleDB Columnar Compression Policy
ALTER TABLE telemetry SET (
    timescaledb.compress,
    timescaledb.compress_segmentby = 'station_id',
    timescaledb.compress_orderby = 'time DESC'
);

-- Automatically compress chunks older than 14 days
SELECT add_compression_policy('telemetry', INTERVAL '14 days', if_not_exists => TRUE);

-- 6. 15-Minute Continuous Aggregate View
CREATE MATERIALIZED VIEW IF NOT EXISTS telemetry_15m
WITH (timescaledb.continuous) AS
SELECT 
    time_bucket('15 minutes', time) AS bucket,
    station_id,
    
    -- Aggregate Temperature
    AVG(temperature_c) AS avg_temperature_c,
    MIN(temperature_c) AS min_temperature_c,
    MAX(temperature_c) AS max_temperature_c,
    
    -- Aggregate Humidity
    AVG(humidity_pct) AS avg_humidity_pct,
    MIN(humidity_pct) AS min_humidity_pct,
    MAX(humidity_pct) AS max_humidity_pct,
    
    -- Aggregate Pressure
    AVG(pressure_hpa) AS avg_pressure_hpa,
    MIN(pressure_hpa) AS min_pressure_hpa,
    MAX(pressure_hpa) AS max_pressure_hpa,
    
    -- Air Quality & System
    AVG(air_quality_ppm) AS avg_air_quality_ppm,
    MIN(battery_mv) AS min_battery_mv,
    AVG(rssi_dbm) AS avg_rssi_dbm,
    
    -- Sample Count & QC Metric (Only include clean readings: qc_flags & 7 == 0)
    COUNT(*) AS total_samples,
    COUNT(*) FILTER (WHERE (qc_flags & 7) = 0) AS clean_samples
FROM telemetry
GROUP BY bucket, station_id
WITH NO DATA;

-- 7. Continuous Aggregate Refresh Policy
-- Refreshes 15m buckets between 2 hours ago and 15 minutes ago every 15 minutes
SELECT add_continuous_aggregate_policy('telemetry_15m',
    start_offset => INTERVAL '2 hours',
    end_offset => INTERVAL '15 minutes',
    schedule_interval => INTERVAL '15 minutes',
    if_not_exists => TRUE
);

-- 8. 1-Hour Continuous Aggregate (Built on raw or 15m)
CREATE MATERIALIZED VIEW IF NOT EXISTS telemetry_1h
WITH (timescaledb.continuous) AS
SELECT 
    time_bucket('1 hour', time) AS bucket,
    station_id,
    AVG(temperature_c) AS avg_temperature_c,
    MIN(temperature_c) AS min_temperature_c,
    MAX(temperature_c) AS max_temperature_c,
    AVG(humidity_pct) AS avg_humidity_pct,
    AVG(pressure_hpa) AS avg_pressure_hpa,
    COUNT(*) AS total_samples
FROM telemetry
WHERE (qc_flags & 7) = 0
GROUP BY bucket, station_id
WITH NO DATA;

SELECT add_continuous_aggregate_policy('telemetry_1h',
    start_offset => INTERVAL '6 hours',
    end_offset => INTERVAL '1 hour',
    schedule_interval => INTERVAL '1 hour',
    if_not_exists => TRUE
);

-- 9. Automated Data Retention Policies
-- Drop uncompressed/compressed raw hypertable partitions older than 90 days
-- Continuous aggregates are preserved long-term
SELECT add_retention_policy('telemetry', INTERVAL '90 days', if_not_exists => TRUE);
