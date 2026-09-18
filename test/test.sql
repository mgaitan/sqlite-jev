.bail on
.headers off
.mode list
.separator |
.load ./build/jev

SELECT jev_config('api_key', 'test-key');
SELECT jev_config('api_url', 'http://127.0.0.1:8765/v1/systemone');
SELECT jev_version();

CREATE TABLE tickets (
  id INTEGER PRIMARY KEY,
  subject TEXT NOT NULL,
  message TEXT NOT NULL
);

INSERT INTO tickets(subject, message) VALUES
  ('Cobro duplicado', 'Me cobraron dos veces. Necesito un reembolso urgente hoy.'),
  ('Error de API', 'La integración devuelve error 500 desde esta mañana.'),
  ('Consulta de plan', 'Quisiera una demo y conocer el precio del plan para equipos.'),
  ('Cambio de tarjeta', '¿Dónde puedo actualizar la tarjeta?');

SELECT round(jev_prob(json_object('message', 'Lo necesito urgente hoy'), 'El mensaje expresa urgencia'), 2);
SELECT jev(json_object('message', 'No hay apuro'), 'El mensaje expresa urgencia');
SELECT jev_choice(
  json_object('subject', 'Error de API'),
  '¿Qué equipo debe resolver este ticket?',
  json_object('billing', 'Cobros y facturas', 'technical', 'Errores e integraciones', 'sales', 'Planes y ventas')
);
SELECT jev_score(
  json_object('message', 'Estoy frustrado pero todavía puedo trabajar'),
  '¿Cuán frustrado está el cliente?',
  json_array('Calmo', 'Frustrado pero respetuoso', 'Muy enojado o agresivo')
);
SELECT jev_score_norm(
  json_object('message', 'Estoy frustrado pero todavía puedo trabajar'),
  '¿Cuán frustrado está el cliente?',
  json_array('Calmo', 'Frustrado pero respetuoso', 'Muy enojado o agresivo')
);
SELECT jev_confidence(
  json_object('message', 'Estoy frustrado pero todavía puedo trabajar'),
  '¿Cuán frustrado está el cliente?',
  'score',
  json_array('Calmo', 'Frustrado pero respetuoso', 'Muy enojado o agresivo')
);

SELECT t.id, round(j.probability, 2)
FROM jev_rows('tickets', 'El ticket expresa urgencia', 'noul', NULL, json_array('subject', 'message')) AS j
JOIN tickets AS t ON t.rowid = j.source_rowid
WHERE j.probability >= 0.5
ORDER BY t.id;

SELECT t.id, j.choice, round(j.confidence, 2)
FROM jev_rows(
  'tickets',
  '¿Qué equipo debe resolver el pedido principal?',
  'choice',
  json_object('billing', 'Cobros, facturas o reembolsos', 'technical', 'Errores o integraciones', 'sales', 'Precios, planes o demos'),
  json_array('subject', 'message')
) AS j
JOIN tickets AS t ON t.rowid = j.source_rowid
ORDER BY t.id;

SELECT json_extract(jev_stats(), '$.requests'), json_extract(jev_stats(), '$.rows_evaluated');

-- Repeating the same batch is served from the connection-local cache.
SELECT count(*) FROM jev_rows('tickets', 'El ticket expresa urgencia', 'noul', NULL, json_array('subject', 'message'));
SELECT json_extract(jev_stats(), '$.requests'), json_extract(jev_stats(), '$.cache_hits');
