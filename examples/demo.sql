-- A small shop schema for trying Data Studio out.
--   sqlite3 demo.db < examples/demo.sql
-- Then add it as a SQLite connection pointing at demo.db.

CREATE TABLE customers (
  id         INTEGER PRIMARY KEY,
  email      TEXT NOT NULL,
  name       TEXT,
  country    TEXT,
  created_at DATETIME
);

CREATE TABLE products (
  id       INTEGER PRIMARY KEY,
  sku      TEXT,
  name     TEXT,
  category TEXT,
  price    REAL
);

CREATE TABLE orders (
  id          INTEGER PRIMARY KEY,
  customer_id INTEGER NOT NULL REFERENCES customers(id),
  status      TEXT,
  total       REAL,
  placed_at   DATETIME
);

CREATE TABLE order_items (
  id         INTEGER PRIMARY KEY,
  order_id   INTEGER REFERENCES orders(id),
  product_id INTEGER REFERENCES products(id),
  qty        INTEGER,
  unit_price REAL
);

CREATE INDEX idx_orders_customer ON orders(customer_id);
CREATE INDEX idx_items_order     ON order_items(order_id);
-- orders.status is deliberately left unindexed so the analyzer has something
-- to find on `SELECT * FROM orders WHERE status = 'paid'`.

INSERT INTO customers (email, name, country, created_at) VALUES
  ('ana@example.com', 'Ana Ruiz',  'ES', '2024-01-05'),
  ('bo@example.com',  'Bo Chen',   'SG', '2024-02-11'),
  ('cy@example.com',  'Cy Patel',  'IN', '2024-03-02'),
  ('di@example.com',  'Di Novak',  'CZ', '2024-03-19');

INSERT INTO products (sku, name, category, price) VALUES
  ('SKU-1', 'Desk Lamp', 'home', 39.00),
  ('SKU-2', 'Keyboard',  'tech', 89.00),
  ('SKU-3', 'Mug',       'home', 12.50),
  ('SKU-4', 'Monitor',   'tech', 249.00);

INSERT INTO orders (customer_id, status, total, placed_at) VALUES
  (1, 'paid',     128.00, '2024-04-01'),
  (1, 'paid',      12.50, '2024-04-15'),
  (2, 'paid',     249.00, '2024-04-20'),
  (3, 'pending',   89.00, '2024-05-02'),
  (4, 'paid',      51.50, '2024-05-08'),
  (2, 'refunded',  39.00, '2024-05-11');

INSERT INTO order_items (order_id, product_id, qty, unit_price) VALUES
  (1, 1, 1,  39.00), (1, 2, 1, 89.00), (2, 3, 1, 12.50),
  (3, 4, 1, 249.00), (4, 2, 1, 89.00), (5, 1, 1, 39.00),
  (5, 3, 1,  12.50), (6, 1, 1, 39.00);
