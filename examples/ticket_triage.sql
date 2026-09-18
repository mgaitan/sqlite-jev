.bail on
.headers on
.mode box
.load ./build/jev

CREATE TABLE support_tickets (
  id INTEGER PRIMARY KEY,
  subject TEXT NOT NULL,
  message TEXT NOT NULL
);

INSERT INTO support_tickets(subject, message) VALUES
  ('Duplicate charge', 'I was charged twice this month. Please reverse one of the charges.'),
  ('Production outage', 'The integration returns HTTP 500 and our work is blocked. We need urgent help.'),
  ('Plan question', 'I would like a demo and pricing for a team of 30 people.'),
  ('Payment card', 'Where can I update the payment card associated with the account?'),
  ('Report export', 'The screen goes blank when I download the report, but I can keep working.'),
  ('Saludo', 'Gracias por la ayuda de ayer, ya quedó resuelto.');

-- A Noul returns P(yes). For the same question P(no) is 1 - P(yes).
-- Picking the larger probability is a 0.5 threshold; here policy uses 0.6.
SELECT 'Yes/no urgency judgment' AS result;
SELECT
  t.id,
  t.subject,
  round(j.probability, 3) AS yes_probability,
  round(1.0 - j.probability, 3) AS no_probability,
  CASE WHEN j.probability >= 0.60 THEN 'yes' ELSE 'no' END AS answer
FROM jev_rows(
  'support_tickets',
  'The customer explicitly expresses urgency or reports that work is blocked',
  'noul',
  NULL,
  json_array('subject', 'message')
) AS j
JOIN support_tickets AS t ON t.rowid = j.source_rowid
ORDER BY t.id;

SELECT 'Tickets requiring urgent attention' AS result;
SELECT t.id, t.subject, round(j.probability, 3) AS probability
FROM jev_rows(
  'support_tickets',
  'The customer explicitly expresses urgency or reports that work is blocked',
  'noul',
  NULL,
  json_array('subject', 'message')
) AS j
JOIN support_tickets AS t ON t.rowid = j.source_rowid
WHERE j.probability >= 0.60
ORDER BY j.probability DESC;

SELECT 'Routing by team' AS result;
SELECT t.id, t.subject, j.choice AS team, round(j.confidence, 3) AS confidence
FROM jev_rows(
  'support_tickets',
  'Which team should handle the ticket''s main request?',
  'choice',
  json_object(
    'billing', 'Charges, invoices, payment cards, or refunds',
    'technical', 'Errors, product failures, or integrations',
    'sales', 'Pricing, plans, demos, or new purchases',
    'other', 'Messages that do not need any of those teams'
  ),
  json_array('subject', 'message')
) AS j
JOIN support_tickets AS t ON t.rowid = j.source_rowid
ORDER BY t.id;

SELECT json(jev_stats()) AS stats;
