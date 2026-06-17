#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Веб-морда для парсера аукціонів UUB.

Запуск (Windows):
    pip install -r requirements.txt
    python app.py

Потім:
  - на цьому ж компі відкрий:  http://localhost:5000
  - з телефона через VPN:      http://<IP-комп'ютера>:5000
    (IP скрипт сам надрукує при старті)
"""

import socket
import traceback

from flask import Flask, request, render_template_string

import uub_parser as parser

app = Flask(__name__)

PAGE = """<!DOCTYPE html>
<html lang="uk">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<meta name="theme-color" content="#0d0a1a">
<title>UUB Парсер — Андрій AI</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    min-height: 100dvh; background: #0d0a1a; color: #f0e9ff;
    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
    background-image:
      radial-gradient(ellipse at 20% 10%, rgba(109,40,217,0.18) 0%, transparent 50%),
      radial-gradient(ellipse at 80% 90%, rgba(76,29,149,0.12) 0%, transparent 50%);
    padding: 18px;
  }
  .wrap { max-width: 640px; margin: 0 auto; }
  .head { display: flex; align-items: center; gap: 12px; margin-bottom: 20px; }
  .avatar {
    width: 42px; height: 42px; border-radius: 50%;
    background: linear-gradient(135deg, #7c3aed, #a78bfa);
    display: flex; align-items: center; justify-content: center;
    font-size: 18px; box-shadow: 0 0 16px rgba(167,139,250,0.35);
  }
  h1 { font-size: 17px; font-weight: 600; }
  .sub { font-size: 12px; color: #a78bfa; }
  form {
    background: rgba(255,255,255,0.04);
    border: 1px solid rgba(167,139,250,0.15);
    border-radius: 16px; padding: 16px; margin-bottom: 18px;
  }
  label { display: block; font-size: 13px; color: #c4b5fd; margin: 10px 0 5px; }
  input {
    width: 100%; background: rgba(255,255,255,0.07);
    border: 1px solid rgba(167,139,250,0.2); border-radius: 12px;
    padding: 12px 14px; color: #f0e9ff; font-size: 15px; outline: none;
  }
  input:focus { border-color: rgba(167,139,250,0.5); }
  button {
    width: 100%; margin-top: 16px; padding: 13px;
    background: linear-gradient(135deg, #7c3aed, #a78bfa);
    border: none; border-radius: 12px; color: #fff; font-size: 16px;
    font-weight: 600; cursor: pointer; box-shadow: 0 3px 12px rgba(124,58,237,0.4);
  }
  button:active { transform: scale(0.98); }
  .card {
    background: rgba(255,255,255,0.05);
    border: 1px solid rgba(167,139,250,0.15);
    border-radius: 16px; padding: 18px; margin-bottom: 14px;
  }
  .verdict { font-size: 18px; font-weight: 700; margin-bottom: 12px; }
  .lot-title { font-size: 16px; font-weight: 600; margin-bottom: 6px; }
  .meta { font-size: 13px; color: #c4b5fd; margin-bottom: 12px; }
  ul { list-style: none; }
  li { font-size: 14px; line-height: 1.7; padding-left: 4px; }
  .links a {
    display: block; padding: 11px 14px; margin-top: 8px;
    background: rgba(124,58,237,0.15); border: 1px solid rgba(167,139,250,0.25);
    border-radius: 10px; color: #c4b5fd; text-decoration: none; font-size: 14px;
  }
  .err {
    background: rgba(239,68,68,0.12); border: 1px solid rgba(239,68,68,0.3);
    color: #fca5a5; border-radius: 12px; padding: 14px; font-size: 14px;
  }
  .hint { font-size: 12px; color: rgba(167,139,250,0.6); margin-top: 6px; }
</style>
</head>
<body>
<div class="wrap">
  <div class="head">
    <div class="avatar">✦</div>
    <div>
      <h1>UUB Парсер аукціонів</h1>
      <div class="sub">аналіз вигідності + перевірка ціни</div>
    </div>
  </div>

  <form method="post" action="/">
    <label>Посилання на лот</label>
    <input name="url" type="url" required placeholder="https://sale.uub.com.ua/auction/..."
           value="{{ url|default('') }}">
    <label>Ринкова ціна, грн (необов'язково)</label>
    <input name="market" type="number" step="any" placeholder="напр. 450000"
           value="{{ market|default('') }}">
    <div class="hint">Якщо вкажеш ринкову ціну — висновок буде точнішим.</div>
    <button type="submit">Аналізувати</button>
  </form>

  {% if error %}
    <div class="err">❌ {{ error }}</div>
  {% endif %}

  {% if result %}
    <div class="card">
      <div class="verdict">{{ result.analysis.verdict }}</div>
      <div class="lot-title">{{ lot.title or '—' }}</div>
      <div class="meta">
        ID: {{ lot.lot_id or '—' }}
        {% if lot.year %} · {{ lot.year }} р.{% endif %}
        {% if lot.mileage_km %} · {{ '{:,.0f}'.format(lot.mileage_km).replace(',', ' ') }} км{% endif %}
      </div>
      <ul>
        {% for n in result.analysis.notes %}<li>• {{ n }}</li>{% endfor %}
      </ul>
    </div>
    <div class="card links">
      <div class="meta">Перевір ринкову ціну (запит: {{ result.market.query }}):</div>
      <a href="{{ result.market['AUTO.RIA'] }}" target="_blank">🚗 AUTO.RIA</a>
      <a href="{{ result.market['OLX'] }}" target="_blank">🛒 OLX</a>
      <a href="{{ result.market['Google'] }}" target="_blank">🔎 Google</a>
    </div>
  {% endif %}
</div>
</body>
</html>"""


@app.route("/", methods=["GET", "POST"])
def index():
    ctx = {}
    if request.method == "POST":
        url = (request.form.get("url") or "").strip()
        market_raw = (request.form.get("market") or "").strip()
        ctx["url"] = url
        ctx["market"] = market_raw
        market = None
        if market_raw:
            try:
                market = float(market_raw)
            except ValueError:
                market = None
        try:
            html = parser.fetch(url)
            lot = parser.extract_lot(html, url)
            analysis = parser.analyze(lot, market)
            links = parser.market_links(lot)
            ctx["lot"] = lot
            ctx["result"] = {"analysis": analysis, "market": links}
        except Exception as e:
            traceback.print_exc()
            ctx["error"] = f"Не вдалося обробити: {e}"
    return render_template_string(PAGE, **ctx)


def _local_ip():
    """Знаходить локальну IP-адресу комп'ютера (для доступу з телефона)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
    except OSError:
        ip = "127.0.0.1"
    finally:
        s.close()
    return ip


if __name__ == "__main__":
    ip = _local_ip()
    port = 5000
    print("=" * 50)
    print("  UUB Парсер запущено!")
    print(f"  На цьому компі:      http://localhost:{port}")
    print(f"  З телефона (VPN/LAN): http://{ip}:{port}")
    print("  Зупинити: Ctrl+C")
    print("=" * 50)
    app.run(host="0.0.0.0", port=port, debug=False)
