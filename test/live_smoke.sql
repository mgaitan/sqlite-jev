.bail on
.headers off
.mode list
.separator |
.load ./build/jev

CREATE TABLE live_tickets (
  id INTEGER PRIMARY KEY,
  subject TEXT NOT NULL,
  message TEXT NOT NULL
);

INSERT INTO live_tickets(subject, message) VALUES
  ('Duplicate charge', 'I was charged twice and would like a refund.'),
  ('Error de integración', 'La API devuelve un error y no podemos continuar trabajando.');

SELECT
  count(*),
  min(j.choice IN ('billing', 'technical', 'other')),
  min(j.confidence BETWEEN 0.0 AND 1.0),
  min(json_valid(j.answer))
FROM jev_rows(
  'live_tickets',
  'Which team should handle the ticket''s main request?',
  'choice',
  json_object(
    'billing', 'Charges, invoices, or refunds',
    'technical', 'Errors, outages, or integrations',
    'other', 'Anything outside billing and technical support'
  ),
  json_array('subject', 'message')
) AS j;

SELECT
  json_extract(jev_stats(), '$.requests'),
  json_extract(jev_stats(), '$.rows_evaluated');
