import React, { useEffect, useRef, useState } from 'react';
import maplibregl, { Map, GeoJSONSource, MapLayerMouseEvent } from 'maplibre-gl';
import 'maplibre-gl/dist/maplibre-gl.css';

export interface WeatherStationGeoJSON {
  type: 'Feature';
  geometry: {
    type: 'Point';
    coordinates: [number, number]; // [lon, lat]
  };
  properties: {
    station_id: string;
    hardware_uid: string;
    topology_type: 'WIFI6_URBAN' | 'SUBGHZ_DECK' | 'OFFGRID_MESH';
    sqi_score: number;
    temperature_c: number;
    humidity_pct: number;
    pressure_hpa: number;
    is_public: boolean;
  };
}

interface MapProps {
  apiEndpoint?: string;
  onSelectStation?: (stationId: string) => void;
}

export const MapboxClusterView: React.FC<MapProps> = ({
  apiEndpoint = '/api/v1/stations/geojson',
  onSelectStation,
}: MapProps) => {
  const mapContainer = useRef<HTMLDivElement>(null);
  const mapRef = useRef<Map | null>(null);
  const [selectedStation, setSelectedStation] = useState<any | null>(null);

  useEffect(() => {
    if (!mapContainer.current) return;

    const map = new maplibregl.Map({
      container: mapContainer.current,
      style: 'https://demotiles.maplibre.org/style.json', // Open-source vector basemap
      center: [13.405, 52.52], // Berlin default
      zoom: 5,
      maxZoom: 18,
    });

    mapRef.current = map;

    map.on('load', () => {
      // 1. Register GeoJSON Source with Native High-Performance Supercluster
      map.addSource('weather-stations', {
        type: 'geojson',
        data: apiEndpoint,
        cluster: true,
        clusterMaxZoom: 14, // Max zoom to cluster points on
        clusterRadius: 50,  // Radius of each cluster when clustering points (defaults to 50)
        clusterProperties: {
          // Track cluster average SQI
          sum_sqi: ['+', ['get', 'sqi_score']],
          min_sqi: ['min', ['get', 'sqi_score']],
        },
      });

      // 2. Clustered Points Layer (Circles)
      map.addLayer({
        id: 'clusters',
        type: 'circle',
        source: 'weather-stations',
        filter: ['has', 'point_count'],
        paint: {
          'circle-color': [
            'step',
            ['get', 'point_count'],
            '#38a169', // <= 100 points
            100,
            '#3182ce', // 101 - 750 points
            750,
            '#805ad5', // > 750 points
          ],
          'circle-radius': [
            'step',
            ['get', 'point_count'],
            18,
            100,
            24,
            750,
            32,
          ],
          'circle-stroke-width': 2,
          'circle-stroke-color': '#ffffff',
          'circle-opacity': 0.85,
        },
      });

      // 3. Cluster Count Numbers
      map.addLayer({
        id: 'cluster-count',
        type: 'symbol',
        source: 'weather-stations',
        filter: ['has', 'point_count'],
        layout: {
          'text-field': '{point_count_abbreviated}',
          'text-font': ['Open Sans Bold'],
          'text-size': 12,
        },
        paint: {
          'text-color': '#ffffff',
        },
      });

      // 4. Unclustered Individual Stations (Color-Coded by SQI)
      // Green >90%, Yellow 60-89%, Red <60%
      map.addLayer({
        id: 'unclustered-point',
        type: 'circle',
        source: 'weather-stations',
        filter: ['!', ['has', 'point_count']],
        paint: {
          'circle-color': [
            'case',
            ['>=', ['get', 'sqi_score'], 90.0],
            '#22c55e', // Green (>90% High Fidelity)
            ['>=', ['get', 'sqi_score'], 60.0],
            '#eab308', // Yellow (60-89% Acceptable / Warning)
            '#ef4444', // Red (<60% Demoted / Spatial Anomaly)
          ],
          'circle-radius': 7,
          'circle-stroke-width': 1.5,
          'circle-stroke-color': '#ffffff',
          'circle-opacity': 0.9,
        },
      });

      // 5. Cluster Click -> Smooth Zoom to Expansion
      map.on('click', 'clusters', async (e: MapLayerMouseEvent) => {
        const features = map.queryRenderedFeatures(e.point, { layers: ['clusters'] });
        const clusterId = features[0]?.properties?.cluster_id;
        const source = map.getSource('weather-stations') as GeoJSONSource;

        if (clusterId !== undefined && source) {
          try {
            const zoom = await source.getClusterExpansionZoom(clusterId);
            const coordinates = (features[0].geometry as any).coordinates;
            map.easeTo({
              center: coordinates,
              zoom: zoom + 0.5,
            });
          } catch (err) {
            console.error('Error getting cluster expansion zoom', err);
          }
        }
      });

      // 6. Individual Station Click -> Telemetry Inspection Modal
      map.on('click', 'unclustered-point', (e: MapLayerMouseEvent) => {
        if (!e.features || e.features.length === 0) return;
        const props = e.features[0].properties;
        setSelectedStation(props);
        if (onSelectStation && props?.station_id) {
          onSelectStation(props.station_id);
        }
      });

      // Pointer cursors
      map.on('mouseenter', 'clusters', () => (map.getCanvas().style.cursor = 'pointer'));
      map.on('mouseleave', 'clusters', () => (map.getCanvas().style.cursor = ''));
      map.on('mouseenter', 'unclustered-point', () => (map.getCanvas().style.cursor = 'pointer'));
      map.on('mouseleave', 'unclustered-point', () => (map.getCanvas().style.cursor = ''));
    });

    return () => {
      map.remove();
    };
  }, [apiEndpoint]);

  return (
    <div style={{ position: 'relative', width: '100%', height: '100vh' }}>
      <div ref={mapContainer} style={{ width: '100%', height: '100%' }} />

      {/* Real-Time SQI Legend */}
      <div
        style={{
          position: 'absolute',
          bottom: 24,
          right: 24,
          background: 'rgba(15, 23, 42, 0.85)',
          backdropFilter: 'blur(8px)',
          color: '#ffffff',
          padding: '12px 18px',
          borderRadius: '8px',
          boxShadow: '0 4px 16px rgba(0,0,0,0.4)',
          fontFamily: 'system-ui, -apple-system, sans-serif',
          fontSize: '13px',
          zIndex: 10,
        }}
      >
        <div style={{ fontWeight: 600, marginBottom: 8 }}>Station Quality Index (SQI)</div>
        <div style={{ display: 'flex', alignItems: 'center', marginBottom: 4 }}>
          <span style={{ width: 10, height: 10, borderRadius: '50%', background: '#22c55e', marginRight: 8 }} />
          <span>&gt; 90% High Reliability (Green)</span>
        </div>
        <div style={{ display: 'flex', alignItems: 'center', marginBottom: 4 }}>
          <span style={{ width: 10, height: 10, borderRadius: '50%', background: '#eab308', marginRight: 8 }} />
          <span>60% - 89% Fair / Warning (Yellow)</span>
        </div>
        <div style={{ display: 'flex', alignItems: 'center' }}>
          <span style={{ width: 10, height: 10, borderRadius: '50%', background: '#ef4444', marginRight: 8 }} />
          <span>&lt; 60% Demoted / Anomaly (Red)</span>
        </div>
      </div>

      {/* Selected Station Inspection Overlay */}
      {selectedStation && (
        <div
          style={{
            position: 'absolute',
            top: 24,
            left: 24,
            width: 300,
            background: 'rgba(15, 23, 42, 0.95)',
            backdropFilter: 'blur(10px)',
            color: '#f8fafc',
            padding: '18px',
            borderRadius: '10px',
            boxShadow: '0 8px 32px rgba(0,0,0,0.5)',
            fontFamily: 'system-ui, sans-serif',
            zIndex: 10,
          }}
        >
          <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
            <h4 style={{ margin: 0, fontSize: '15px', color: '#38bdf8' }}>{selectedStation.hardware_uid}</h4>
            <button
              onClick={() => setSelectedStation(null)}
              style={{ background: 'transparent', border: 'none', color: '#94a3b8', cursor: 'pointer', fontSize: '16px' }}
            >
              x
            </button>
          </div>
          <div style={{ fontSize: '12px', color: '#94a3b8', margin: '4px 0 12px 0' }}>
            Topology: {selectedStation.topology_type}
          </div>
          <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '8px', fontSize: '13px' }}>
            <div>Temp: <strong>{selectedStation.temperature_c}°C</strong></div>
            <div>Humidity: <strong>{selectedStation.humidity_pct}%</strong></div>
            <div>Pressure: <strong>{selectedStation.pressure_hpa} hPa</strong></div>
            <div>SQI: <strong>{selectedStation.sqi_score}%</strong></div>
          </div>
        </div>
      )}
    </div>
  );
};
