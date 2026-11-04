-- Demo zone example.test., loaded once by docker-entrypoint.sh right after the tables are created
-- (compose mounts it as /docker-entrypoint.d/10-demo.sql). Same data as tests/functional/smoke.sh.

-- TXT data longer than 255 octets, or several strings (separated by NUL), needs room and bytes.
ALTER TABLE rr MODIFY data VARBINARY(1024) NOT NULL;

INSERT INTO soa (origin, ns, mbox, serial, refresh, retry, expire, minimum, ttl)
VALUES ('example.test.', 'ns1.example.test.', 'admin.example.test.', 1, 28800, 7200, 604800, 86400, 3600);
SET @zone = LAST_INSERT_ID();

INSERT INTO rr (zone, name, type, data, aux, ttl) VALUES
  (@zone, '',          'NS',    'ns1.example.test.',       0, 3600),
  (@zone, 'ns1',       'A',     '192.0.2.53',              0, 3600),
  (@zone, 'www',       'A',     '192.0.2.10',              0, 3600),
  (@zone, 'www',       'AAAA',  '2001:db8::10',            0, 3600),
  (@zone, '',          'MX',    'mail.example.test.',     10, 3600),
  (@zone, 'mail',      'A',     '192.0.2.25',              0, 3600),
  (@zone, 'alias',     'CNAME', 'www.example.test.',       0, 3600),
  (@zone, 'short',     'TXT',   'hello world',             0, 3600),
  (@zone, '_sip._tcp', 'SRV',   '0 5060 sip.example.test.', 10, 3600),
  (@zone, 'multi',     'TXT',   CONCAT(REPEAT('a', 200), CHAR(0), REPEAT('b', 200)), 0, 3600);
