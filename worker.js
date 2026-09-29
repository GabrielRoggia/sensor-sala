// worker.js — Sensor de sala (IoT) — Cloudflare Worker + D1
// Binding do banco D1 no Worker: DB
//
// POST /movimento          -> grava um movimento (ts = Date.now())
// POST /luz                -> grava leitura de luz { "valor": 3120, "acesa": 1 }
// GET  /ultimo-movimento   -> há quantos segundos houve o último movimento
// GET  /por-hora           -> movimentos por hora do dia (horário de Brasília)
// GET  /luz-resumo         -> total de leituras, quantas com luz acesa e %
// GET  /desperdicio        -> leituras com luz acesa sem movimento nos 2 min anteriores

const JANELA_DESPERDICIO_MS = 120000; // 2 minutos

const SQL = {
  insereMovimento: "INSERT INTO movimentos (ts) VALUES (?)",
  insereLuz: "INSERT INTO luz (valor, acesa, ts) VALUES (?, ?, ?)",

  ultimoMovimento: "SELECT MAX(ts) AS ts FROM movimentos",

  porHora: `
    SELECT strftime('%H', ts / 1000, 'unixepoch', '-3 hours') AS hora,
           COUNT(*) AS movimentos
    FROM movimentos
    GROUP BY hora
    ORDER BY hora`,

  luzResumo: `
    SELECT COUNT(*) AS total,
           COALESCE(SUM(CASE WHEN acesa = 1 THEN 1 ELSE 0 END), 0) AS acesas,
           COALESCE(ROUND(100.0 * SUM(CASE WHEN acesa = 1 THEN 1 ELSE 0 END) / COUNT(*), 1), 0) AS porcentagem
    FROM luz`,

  desperdicio: `
    SELECT COUNT(*) AS desperdicio
    FROM luz
    WHERE acesa = 1
      AND NOT EXISTS (
        SELECT 1 FROM movimentos m
        WHERE m.ts BETWEEN luz.ts - ? AND luz.ts
      )`,
};

function json(data, status = 200) {
  return new Response(JSON.stringify(data), {
    status,
    headers: {
      "Content-Type": "application/json; charset=utf-8",
      "Access-Control-Allow-Origin": "*",
    },
  });
}

async function postMovimento(env) {
  const ts = Date.now();
  const r = await env.DB.prepare(SQL.insereMovimento).bind(ts).run();
  return json({ ok: true, id: r.meta.last_row_id, ts }, 201);
}

async function postLuz(request, env) {
  let body;
  try {
    body = await request.json();
  } catch {
    return json({ erro: "JSON inválido" }, 400);
  }

  const valor = Number(body.valor);
  const acesa = Number(body.acesa);
  if (!Number.isInteger(valor) || valor < 0 || valor > 4095) {
    return json({ erro: "'valor' deve ser inteiro entre 0 e 4095" }, 400);
  }
  if (acesa !== 0 && acesa !== 1) {
    return json({ erro: "'acesa' deve ser 0 ou 1" }, 400);
  }

  const ts = Date.now();
  const r = await env.DB.prepare(SQL.insereLuz).bind(valor, acesa, ts).run();
  return json({ ok: true, id: r.meta.last_row_id, valor, acesa, ts }, 201);
}

async function getUltimoMovimento(env) {
  const row = await env.DB.prepare(SQL.ultimoMovimento).first();
  if (!row || row.ts === null) {
    return json({ erro: "nenhum movimento registrado" }, 404);
  }
  const segundos = Math.floor((Date.now() - row.ts) / 1000);
  return json({ ultimo_ts: row.ts, segundos_atras: segundos });
}

async function getPorHora(env) {
  const { results } = await env.DB.prepare(SQL.porHora).all();
  return json({ fuso: "America/Sao_Paulo (UTC-3)", por_hora: results });
}

async function getLuzResumo(env) {
  const row = await env.DB.prepare(SQL.luzResumo).first();
  return json(row);
}

async function getDesperdicio(env) {
  const row = await env.DB.prepare(SQL.desperdicio).bind(JANELA_DESPERDICIO_MS).first();
  return json({ janela_minutos: 2, leituras_desperdicio: row.desperdicio });
}

const ROTAS = {
  "/movimento": { POST: (req, env) => postMovimento(env) },
  "/luz": { POST: (req, env) => postLuz(req, env) },
  "/ultimo-movimento": { GET: (req, env) => getUltimoMovimento(env) },
  "/por-hora": { GET: (req, env) => getPorHora(env) },
  "/luz-resumo": { GET: (req, env) => getLuzResumo(env) },
  "/desperdicio": { GET: (req, env) => getDesperdicio(env) },
};

export default {
  async fetch(request, env) {
    const { pathname } = new URL(request.url);
    const rota = ROTAS[pathname];

    if (!rota) return json({ erro: "rota não encontrada", rotas: Object.keys(ROTAS) }, 404);

    const handler = rota[request.method];
    if (!handler) return json({ erro: `método ${request.method} não permitido` }, 405);

    try {
      return await handler(request, env);
    } catch (e) {
      return json({ erro: "erro interno", detalhe: String(e) }, 500);
    }
  },
};
