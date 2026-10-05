"""
Multi-Tier Quality Control (QC) & Spatial Inverse Distance Weighting (IDW) Engine
Part of the Crowdsourced Open-Source Weather Ecosystem

Pipeline Stages:
  Tier 1: Physical Hardware Boundary Filters
  Tier 2: Temporal Rate-of-Change Filter (Sun Strike / Sensor Glitch Detection)
  Tier 3: Spatial IDW Outlier Detection (3-Sigma Weighted Variance on N<=10 Neighbors, R<=25km)
  Tier 4: Station Quality Index (SQI) State Tracking & API Demotion Logic
"""

from __future__ import annotations
import math
from dataclasses import dataclass, field
from datetime import datetime, timezone
from enum import IntFlag
from typing import List, Optional, Tuple


class QCFlag(IntFlag):
    CLEAN = 0
    HARDWARE_LIMIT_FAIL = 1 << 0  # 0x01: Impossible physics
    TEMPORAL_DELTA_FAIL = 1 << 1  # 0x02: Glitch or direct sun strike (>3C/60s)
    SPATIAL_ANOMALY = 1 << 2      # 0x04: Diverged > 3-sigma from weighted spatial cluster
    CRYPTOGRAPHIC_FAIL = 1 << 3   # 0x08: Signature validation failed


@dataclass(frozen=True)
class HardwareLimits:
    MIN_TEMP_C: float = -80.0
    MAX_TEMP_C: float = 70.0
    MIN_PRESSURE_HPA: float = 800.0
    MAX_PRESSURE_HPA: float = 1100.0
    MIN_HUMIDITY_PCT: float = 0.0
    MAX_HUMIDITY_PCT: float = 100.0
    MIN_BATTERY_MV: int = 2500
    MAX_BATTERY_MV: int = 3700


@dataclass
class TelemetrySample:
    station_id: str
    timestamp: datetime
    latitude: float
    longitude: float
    temperature_c: float
    humidity_pct: float
    pressure_hpa: float
    battery_mv: int
    air_quality_ppm: Optional[float] = None


@dataclass
class NeighborStationData:
    station_id: str
    latitude: float
    longitude: float
    temperature_c: float
    sqi_score: float  # Only neighbors with SQI >= 60.0 are admitted


@dataclass
class QCResult:
    passed: bool
    qc_flags: QCFlag
    target_temp: float
    predicted_spatial_mean: Optional[float]
    weighted_std_dev: Optional[float]
    z_score: Optional[float]
    adjusted_sqi: float
    is_public: bool
    rejection_reason: Optional[str] = None


class QualityControlEngine:
    def __init__(
        self,
        limits: HardwareLimits = HardwareLimits(),
        max_search_radius_km: float = 25.0,
        max_neighbors: int = 10,
        sigma_threshold: float = 3.0,
        sqi_anomaly_penalty: float = 5.0,
        sqi_clean_24h_reward: float = 1.0,
        min_spatial_sigma: float = 0.5,  # Prevents division by zero or false positive when all neighbors are identical
    ):
        self.limits = limits
        self.max_search_radius_km = max_search_radius_km
        self.max_neighbors = max_neighbors
        self.sigma_threshold = sigma_threshold
        self.sqi_anomaly_penalty = sqi_anomaly_penalty
        self.sqi_clean_24h_reward = sqi_clean_24h_reward
        self.min_spatial_sigma = min_spatial_sigma

    @staticmethod
    def haversine_distance_km(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
        """Calculates Great-Circle distance between two coordinates in kilometers."""
        r_earth = 6371.0088  # Mean Earth radius in km
        phi1 = math.radians(lat1)
        phi2 = math.radians(lat2)
        delta_phi = math.radians(lat2 - lat1)
        delta_lambda = math.radians(lon2 - lon1)

        a = (
            math.sin(delta_phi / 2.0) ** 2
            + math.cos(phi1) * math.cos(phi2) * math.sin(delta_lambda / 2.0) ** 2
        )
        c = 2.0 * math.atan2(math.sqrt(a), math.sqrt(1.0 - a))
        return r_earth * c

    def evaluate_hardware_limits(self, sample: TelemetrySample) -> Tuple[bool, Optional[str]]:
        """Tier 1: Verify values adhere to physical atmospheric and hardware boundaries."""
        if not (self.limits.MIN_TEMP_C <= sample.temperature_c <= self.limits.MAX_TEMP_C):
            return False, f"Temperature {sample.temperature_c}C violates limits [{self.limits.MIN_TEMP_C}, {self.limits.MAX_TEMP_C}]"

        if not (self.limits.MIN_PRESSURE_HPA <= sample.pressure_hpa <= self.limits.MAX_PRESSURE_HPA):
            return False, f"Pressure {sample.pressure_hpa}hPa violates limits [{self.limits.MIN_PRESSURE_HPA}, {self.limits.MAX_PRESSURE_HPA}]"

        if not (self.limits.MIN_HUMIDITY_PCT <= sample.humidity_pct <= self.limits.MAX_HUMIDITY_PCT):
            return False, f"Humidity {sample.humidity_pct}% violates limits [{self.limits.MIN_HUMIDITY_PCT}, {self.limits.MAX_HUMIDITY_PCT}]"

        if not (self.limits.MIN_BATTERY_MV <= sample.battery_mv <= self.limits.MAX_BATTERY_MV):
            return False, f"Battery {sample.battery_mv}mV violates bounds [{self.limits.MIN_BATTERY_MV}, {self.limits.MAX_BATTERY_MV}]"

        return True, None

    def evaluate_temporal_filter(
        self,
        current_sample: TelemetrySample,
        previous_sample: Optional[TelemetrySample],
    ) -> Tuple[bool, Optional[str]]:
        """
        Tier 2: Temporal Delta Filter.
        Flags direct solar strikes or thermal shocks: Drop if |ΔT| > 3.0°C within 60 seconds.
        Normalizes rate of change if time delta differs.
        """
        if previous_sample is None:
            return True, None

        delta_t_sec = abs((current_sample.timestamp - previous_sample.timestamp).total_seconds())
        if delta_t_sec <= 0.0:
            return False, "Duplicate or non-advancing timestamp"

        delta_temp = abs(current_sample.temperature_c - previous_sample.temperature_c)

        # Base specification: > 3.0 C within 60 seconds (rate > 0.05 C/sec)
        if delta_t_sec <= 60.0:
            if delta_temp > 3.0:
                return False, f"Temporal thermal surge: ΔT={delta_temp:.2f}C within {delta_t_sec:.1f}s"
        else:
            # Scaled tolerance for longer intervals (e.g., maximum allowable rate of 0.05 C/sec capped at 6C)
            max_allowed = min(6.0, 3.0 * (delta_t_sec / 60.0))
            if delta_temp > max_allowed:
                return False, f"Temporal rate exceeded: ΔT={delta_temp:.2f}C across {delta_t_sec:.1f}s"

        return True, None

    def evaluate_spatial_idw(
        self,
        target: TelemetrySample,
        neighbors: List[NeighborStationData],
    ) -> Tuple[bool, Optional[float], Optional[float], Optional[float], Optional[str]]:
        """
        Tier 3: Spatial Inverse Distance Weighting (IDW) with weighted sample variance.
        Cross-validates target temperature against up to N<=10 neighbors within 25km.
        Weight: w_i = 1 / (d_i^2)
        Calculates:
          - Weighted Mean: μ = (Σ w_i * x_i) / (Σ w_i)
          - Weighted Variance: σ^2 = [Σ w_i(x_i - μ)^2] / [Σ w_i - (Σ w_i^2 / Σ w_i)] (Unbiased reliability weighting)
          - Threshold: |x_target - μ| > 3 * σ -> SPATIAL_ANOMALY
        """
        # Filter candidate neighbors: within search radius, non-zero SQI, exclude self
        candidates: List[Tuple[float, float]] = []  # (distance_km, temperature_c)

        for n in neighbors:
            if n.station_id == target.station_id:
                continue
            if n.sqi_score < 60.0:
                continue  # Exclude untrusted/demoted neighbor stations

            d_km = self.haversine_distance_km(target.latitude, target.longitude, n.latitude, n.longitude)
            if d_km <= self.max_search_radius_km:
                candidates.append((d_km, n.temperature_c))

        # Sort by distance and select top N
        candidates.sort(key=lambda item: item[0])
        selected = candidates[: self.max_neighbors]

        # If sparse network (fewer than 3 trusted neighbors), cannot form statistical consensus
        if len(selected) < 3:
            return True, None, None, None, None

        # Calculate IDW Weights: w_i = 1 / (d_i^2)
        # Apply epsilon floor (50 meters = 0.05 km) to avoid singularity when collocated
        eps_km = 0.05
        weights: List[float] = []
        temps: List[float] = []

        for d_km, temp in selected:
            effective_d = max(d_km, eps_km)
            w = 1.0 / (effective_d**2)
            weights.append(w)
            temps.append(temp)

        sum_w = sum(weights)
        sum_w_sq = sum(w**2 for w in weights)

        # Weighted Mean: μ
        weighted_mean = sum(w * t for w, t in zip(weights, temps)) / sum_w

        # Reliability-corrected Weighted Variance (West's / Galassi formulation)
        denominator = sum_w - (sum_w_sq / sum_w)
        if denominator <= 0:
            weighted_var = 0.0
        else:
            weighted_var = sum(w * ((t - weighted_mean) ** 2) for w, t in zip(weights, temps)) / denominator

        # Impose sensor/microclimate noise floor to avoid false positives in uniform weather
        std_dev = max(math.sqrt(max(0.0, weighted_var)), self.min_spatial_sigma)

        deviation = abs(target.temperature_c - weighted_mean)
        z_score = deviation / std_dev

        if z_score > self.sigma_threshold:
            reason = (
                f"Spatial Anomaly: Target={target.temperature_c:.2f}C, "
                f"IDW Mean={weighted_mean:.2f}C, sigma={std_dev:.2f}C, z={z_score:.2f} > {self.sigma_threshold}"
            )
            return False, weighted_mean, std_dev, z_score, reason

        return True, weighted_mean, std_dev, z_score, None

    def calculate_sqi_update(
        self,
        current_sqi: float,
        is_clean_reading: bool,
        last_clean_timestamp: datetime,
        current_timestamp: datetime,
    ) -> Tuple[float, bool]:
        """
        Tier 4: Station Quality Index (SQI) State Progression.
        Base: 100.0%.
        Deduction: -5% per spatial anomaly.
        Reward: +1% per 24 hours of clean continuous runtime.
        Demotion: SQI < 60% demotes station from public dissemination.
        """
        score = current_sqi

        if not is_clean_reading:
            # Penalize
            score = max(0.0, score - self.sqi_anomaly_penalty)
        else:
            # Check elapsed clean time
            elapsed_sec = (current_timestamp - last_clean_timestamp).total_seconds()
            if elapsed_sec >= 86400.0:  # 24 hours
                full_days = int(elapsed_sec // 86400.0)
                reward = full_days * self.sqi_clean_24h_reward
                score = min(100.0, score + reward)

        is_public = score >= 60.0
        return round(score, 2), is_public

    def process_telemetry(
        self,
        sample: TelemetrySample,
        previous_sample: Optional[TelemetrySample],
        neighbors: List[NeighborStationData],
        current_sqi: float,
        last_clean_timestamp: datetime,
    ) -> QCResult:
        """Full end-to-end pipeline execution for an incoming telemetry packet."""
        flags = QCFlag.CLEAN
        rejection_reasons = []

        # 1. Hardware Limits
        hw_ok, hw_msg = self.evaluate_hardware_limits(sample)
        if not hw_ok:
            flags |= QCFlag.HARDWARE_LIMIT_FAIL
            rejection_reasons.append(hw_msg)

        # 2. Temporal Filter
        temp_ok, temp_msg = self.evaluate_temporal_filter(sample, previous_sample)
        if not temp_ok:
            flags |= QCFlag.TEMPORAL_DELTA_FAIL
            rejection_reasons.append(temp_msg)

        # 3. Spatial IDW (only run if sample is physically sane)
        pred_mean, std_dev, z_score = None, None, None
        if hw_ok:
            spatial_ok, pred_mean, std_dev, z_score, spatial_msg = self.evaluate_spatial_idw(sample, neighbors)
            if not spatial_ok:
                flags |= QCFlag.SPATIAL_ANOMALY
                rejection_reasons.append(spatial_msg)

        # 4. SQI Update
        is_clean = (flags == QCFlag.CLEAN)
        new_sqi, is_public = self.calculate_sqi_update(
            current_sqi=current_sqi,
            is_clean_reading=is_clean,
            last_clean_timestamp=last_clean_timestamp,
            current_timestamp=sample.timestamp,
        )

        return QCResult(
            passed=is_clean,
            qc_flags=flags,
            target_temp=sample.temperature_c,
            predicted_spatial_mean=pred_mean,
            weighted_std_dev=std_dev,
            z_score=z_score,
            adjusted_sqi=new_sqi,
            is_public=is_public,
            rejection_reason="; ".join(rejection_reasons) if rejection_reasons else None,
        )


# ==============================================================================
# SELF-TEST & VALIDATION SUITE
# ==============================================================================
if __name__ == "__main__":
    qc = QualityControlEngine()
    now = datetime.now(timezone.utc)

    # Center station: Berlin Alexanderplatz (52.5200, 13.4050)
    target_station = TelemetrySample(
        station_id="station-target-01",
        timestamp=now,
        latitude=52.5200,
        longitude=13.4050,
        temperature_c=34.8,  # Rogue reading (real ambient is ~21.5C)
        humidity_pct=45.0,
        pressure_hpa=1013.25,
        battery_mv=3280,
    )

    # 5 surrounding stations within 2-15km reporting coherent ~21.0 - 22.0 C
    neighbors = [
        NeighborStationData("st-02", 52.5163, 13.3777, 21.2, 98.0), # Tiergarten (~2.5km)
        NeighborStationData("st-03", 52.5350, 13.4170, 21.6, 95.0), # Prenzlauer Berg (~2.0km)
        NeighborStationData("st-04", 52.4820, 13.4350, 21.4, 91.0), # Neukolln (~5.0km)
        NeighborStationData("st-05", 52.5500, 13.3500, 22.0, 88.0), # Wedding (~5.5km)
        NeighborStationData("st-06", 52.5000, 13.5000, 21.1, 99.0), # Lichtenberg (~7.0km)
    ]

    result = qc.process_telemetry(
        sample=target_station,
        previous_sample=None,
        neighbors=neighbors,
        current_sqi=100.0,
        last_clean_timestamp=now,
    )

    print("=== Spatial IDW QC Engine Verification ===")
    print(f"Target Temp:          {result.target_temp} C")
    print(f"Predicted IDW Mean:   {result.predicted_spatial_mean:.2f} C")
    print(f"Weighted Std Dev (sigma): {result.weighted_std_dev:.2f} C")
    print(f"Z-Score:              {result.z_score:.2f} sigma")
    print(f"Flags:                {bin(result.qc_flags)} ({result.qc_flags.name})")
    print(f"Adjusted SQI:         {result.adjusted_sqi}% (Public: {result.is_public})")
    print(f"Rejection Reason:     {result.rejection_reason}")
    assert result.qc_flags & QCFlag.SPATIAL_ANOMALY, "Must catch spatial anomaly!"
    assert result.adjusted_sqi == 95.0, "Must deduct 5% SQI!"
    print("Verification Passed successfully!")
