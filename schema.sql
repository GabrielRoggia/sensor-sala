-- Banco D1 — tabelas do sensor de sala
CREATE TABLE IF NOT EXISTS movimentos (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  ts INTEGER NOT NULL            -- Date.now() do Worker, em ms
);

CREATE TABLE IF NOT EXISTS luz (
  id     INTEGER PRIMARY KEY AUTOINCREMENT,
  valor  INTEGER NOT NULL,       -- analogRead(34): 0 a 4095
  acesa  INTEGER NOT NULL,       -- 1 = luz acesa, 0 = apagada
  ts     INTEGER NOT NULL        -- Date.now() do Worker, em ms
);

CREATE INDEX IF NOT EXISTS idx_movimentos_ts ON movimentos(ts);
CREATE INDEX IF NOT EXISTS idx_luz_ts ON luz(ts);
