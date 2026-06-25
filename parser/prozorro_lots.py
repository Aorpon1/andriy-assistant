#!/usr/bin/env python3
"""Аналізатор лотів Prozorro.Sale.

Збирає активні (доступні до участі) лоти з публічного API Prozorro.Sale,
оцінює вигідність кожного і будує зрозумілий звіт: CSV + інтерактивний HTML.

ЯК РАХУЄТЬСЯ "ВИГІДНІСТЬ"
------------------------
У Prozorro.Sale поле лота `value` — це майже завжди СТАРТОВА ЦІНА аукціону,
а окремої "незалежної ринкової оцінки" в API немає. Тож інструмент будує
"ринкову вартість" (market value) з трьох джерел, у порядку надійності:

  1. Твій файл довідкових цін (--reference-file) — категорія/слово → типова
     ринкова ціна. Це найточніше, бо ти задаєш реальний ринок.
  2. Ціна попереднього (несостоявшегося) аукціону для повторних торгів зі
     знижкою — поле `discount.previousAuctionValue`.
  3. Якщо нічого з цього немає — ринкова вартість невідома, лот позначається
     як NEEDREF (треба довідкова ціна), але все одно потрапляє в таблицю з
     корисними сигналами (знижка, тип аукціону, ставки, дедлайн).

Запуск (на машині з вільним інтернетом):
    pip install -r requirements.txt
    python prozorro_lots.py --max-pages 20

Приклади:
    # тільки авто, останні 14 днів, з власними довідковими цінами
    python prozorro_lots.py --keyword авто --since-days 14 --reference-file prices.csv

    # подивитись сиру структуру одного лота (якщо поля API зміняться)
    python prozorro_lots.py --debug-one
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import html
import json
import re
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, asdict
from pathlib import Path

try:
    import requests
except ImportError:
    sys.exit("Потрібен пакет 'requests'. Встанови: pip install -r requirements.txt")


BASE = "https://public-api.prozorro.sale"
FEED_PATH = "/api/v1/auctions"
DETAIL_PATH = "/api/v1/auctions/{id}"

# Статуси, у яких лот ще можна купити / взяти участь.
ACTIVE_STATUSES = {"active_rectification", "active_tendering", "active_auction"}

OUT_DIR = Path(__file__).resolve().parent / "output"


@dataclass
class Lot:
    auction_id: str
    title: str
    category: str
    status: str
    method: str
    currency: str
    start_price: float        # стартова ціна аукціону (надійне поле value.amount)
    market_value: float       # оцінка ринкової вартості (з довідника / попередніх торгів)
    market_source: str        # звідки взяли ринкову вартість
    prev_value: float         # ціна попереднього аукціону (повторні торги)
    discount_pct_listed: float  # офіційна знижка повторного аукціону, %
    min_step: float
    guarantee: float
    bids: int
    region: str
    is_falling: bool          # голландський аукціон — ціна падає з часом
    end_date: str
    url: str
    # обчислювані поля
    margin_pct: float = 0.0   # наскільки старт нижчий за ринкову вартість, %
    max_bid: float = 0.0      # максимум, за який варто думати про покупку
    potential_gain: float = 0.0
    verdict: str = ""         # GOOD / WATCH / SKIP / NEEDREF
    note: str = ""


# ---------------------------------------------------------------------------
# Мережа
# ---------------------------------------------------------------------------

def session() -> requests.Session:
    s = requests.Session()
    s.headers.update({"User-Agent": "prozorro-lots-analyzer/1.0"})
    return s


def fetch_json(s: requests.Session, url: str, params: dict | None = None) -> dict:
    last_err = None
    for _ in range(4):
        try:
            r = s.get(url, params=params, timeout=30)
            r.raise_for_status()
            return r.json()
        except Exception as e:  # noqa: BLE001
            last_err = e
    raise RuntimeError(f"Не вдалося отримати {url}: {last_err}")


def walk_feed(s: requests.Session, max_pages: int, since: dt.datetime | None) -> list[str]:
    """Йде стрічкою аукціонів від найновіших до старіших, віддає id активних лотів."""
    params = {"descending": 1, "limit": 100}
    active: list[str] = []
    offset = None
    for _ in range(max_pages):
        if offset:
            params["offset"] = offset
        data = fetch_json(s, BASE + FEED_PATH, params)
        items = data.get("data", [])
        if not items:
            break

        oldest = None
        for it in items:
            oldest = it.get("dateModified", "") or oldest
            status = it.get("status")  # у стрічці може бути статус — фільтруємо одразу
            if status is not None and status not in ACTIVE_STATUSES:
                continue
            active.append(it["id"])

        offset = (data.get("next_page") or {}).get("offset")
        if since and oldest:
            try:
                if dt.datetime.fromisoformat(oldest.replace("Z", "+00:00")) < since:
                    break
            except ValueError:
                pass
        if not offset:
            break
    return list(dict.fromkeys(active))


# ---------------------------------------------------------------------------
# Розбір лота
# ---------------------------------------------------------------------------

FALLING_HINTS = ("dutch", "priceQuotation")


def _num(x) -> float:
    try:
        return float(x)
    except (TypeError, ValueError):
        return 0.0


def parse_auction(raw: dict) -> Lot | None:
    a = raw.get("data", raw)
    status = a.get("status", "")
    if status and status not in ACTIVE_STATUSES:
        return None

    value = a.get("value") or {}
    step = a.get("minimalStep") or {}
    guar = a.get("guarantee") or {}
    discount = a.get("discount") or {}

    items = a.get("items") or []
    category = region = ""
    if items:
        cls = items[0].get("classification") or {}
        category = (cls.get("description") or cls.get("id") or "").strip()
        addr = items[0].get("address") or {}
        region = (addr.get("region") or addr.get("locality") or "").strip()

    period = a.get("auctionPeriod") or a.get("rectificationPeriod") or {}
    end_date = period.get("endDate") or period.get("startDate") or ""

    bids = a.get("bids")
    bids_n = len(bids) if isinstance(bids, list) else 0

    method = a.get("procurementMethodType", "")
    auction_id = a.get("auctionId") or a.get("id", "")

    return Lot(
        auction_id=auction_id,
        title=(a.get("title") or "").strip(),
        category=category,
        status=status,
        method=method,
        currency=value.get("currency", "UAH"),
        start_price=_num(value.get("amount")),
        market_value=0.0,
        market_source="",
        prev_value=_num(discount.get("previousAuctionValue")),
        discount_pct_listed=_num(discount.get("discountPercent")),
        min_step=_num(step.get("amount")),
        guarantee=_num(guar.get("amount")),
        bids=bids_n,
        region=region,
        is_falling=any(h.lower() in method.lower() for h in FALLING_HINTS),
        end_date=end_date,
        url=f"https://prozorro.sale/auction/{auction_id}" if auction_id else "",
    )


# ---------------------------------------------------------------------------
# Довідкові ціни (опціонально) та аналіз
# ---------------------------------------------------------------------------

def load_reference(path: Path | None) -> list[tuple[re.Pattern, float]]:
    """CSV із двох колонок: keyword,price. keyword шукається у назві/категорії."""
    if not path:
        return []
    refs: list[tuple[re.Pattern, float]] = []
    with path.open(encoding="utf-8-sig") as f:
        for row in csv.reader(f):
            if len(row) < 2:
                continue
            kw, price = row[0].strip(), _num(row[1])
            if kw and price > 0:
                refs.append((re.compile(re.escape(kw), re.IGNORECASE), price))
    return refs


def resolve_market_value(lot: Lot, refs) -> None:
    hay = f"{lot.title} {lot.category}"
    for pat, price in refs:
        if pat.search(hay):
            lot.market_value, lot.market_source = price, f"довідник: {pat.pattern}"
            return
    if lot.prev_value > 0:
        lot.market_value, lot.market_source = lot.prev_value, "ціна попередніх торгів"


def analyze(lot: Lot, refs, target_discount: float) -> Lot:
    """Вердикт + максимальна ціна покупки.

    target_discount=0.30 → купуємо, лише якщо можна взяти щонайменше на 30%
    дешевше за ринкову вартість (запас маржі).
    """
    resolve_market_value(lot, refs)
    mv, start = lot.market_value, lot.start_price

    if mv > 0:
        lot.margin_pct = round((mv - start) / mv * 100, 1)
        lot.max_bid = round(mv * (1 - target_discount), 2)
        lot.potential_gain = round(mv - lot.max_bid, 2)
        if start <= lot.max_bid:
            lot.verdict = "GOOD"
            lot.note = f"старт на {lot.margin_pct:.0f}% нижче ринку ({lot.market_source})"
        elif start < mv:
            lot.verdict = "WATCH"
            lot.note = f"на {lot.margin_pct:.0f}% нижче ринку, але вище цільового запасу"
        else:
            lot.verdict = "SKIP"
            lot.note = f"старт ≥ ринкової вартості ({lot.market_source})"
        if lot.verdict == "GOOD" and lot.bids >= 3:
            lot.note += f"; уже {lot.bids} ставок"
    else:
        # Ринкова вартість невідома — даємо корисні сигнали без вердикту "купувати".
        lot.verdict = "NEEDREF"
        signals = []
        if lot.discount_pct_listed > 0:
            signals.append(f"повторні торги, знижка {lot.discount_pct_listed:.0f}%")
        if lot.is_falling:
            signals.append("голландський — ціна падає")
        if lot.bids:
            signals.append(f"{lot.bids} ставок")
        lot.note = "; ".join(signals) or "немає довідкової ціни для порівняння"

    return lot


VERDICT_ORDER = {"GOOD": 0, "WATCH": 1, "NEEDREF": 2, "SKIP": 3}


# ---------------------------------------------------------------------------
# Збір
# ---------------------------------------------------------------------------

def collect(args, refs) -> list[Lot]:
    s = session()
    since = None
    if args.since_days:
        since = dt.datetime.now(dt.timezone.utc) - dt.timedelta(days=args.since_days)

    print(f"Збираю стрічку аукціонів (до {args.max_pages} сторінок)…", file=sys.stderr)
    ids = walk_feed(s, args.max_pages, since)
    print(f"Знайдено кандидатів: {len(ids)}. Тягну деталі…", file=sys.stderr)

    lots: list[Lot] = []
    with ThreadPoolExecutor(max_workers=args.workers) as ex:
        futs = {ex.submit(fetch_json, s, BASE + DETAIL_PATH.format(id=i)): i for i in ids}
        for n, fut in enumerate(as_completed(futs), 1):
            try:
                lot = parse_auction(fut.result())
            except Exception as e:  # noqa: BLE001
                print(f"  пропуск {futs[fut]}: {e}", file=sys.stderr)
                continue
            if not lot:
                continue
            if args.keyword and args.keyword.lower() not in f"{lot.title} {lot.category}".lower():
                continue
            lots.append(analyze(lot, refs, args.target_discount))
            if n % 50 == 0:
                print(f"  оброблено {n}/{len(ids)}…", file=sys.stderr)

    lots.sort(key=lambda l: (VERDICT_ORDER.get(l.verdict, 9), -l.potential_gain, -l.discount_pct_listed))
    return lots


# ---------------------------------------------------------------------------
# Звіти
# ---------------------------------------------------------------------------

def write_csv(lots: list[Lot], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = [f.name for f in Lot.__dataclass_fields__.values()]
    with path.open("w", newline="", encoding="utf-8-sig") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for lot in lots:
            w.writerow(asdict(lot))


BADGE = {"GOOD": "🟢 Варто", "WATCH": "🟡 Стежити", "NEEDREF": "⚪ Треба ціна", "SKIP": "🔴 Пропустити"}
CSS_CLASS = {"GOOD": "good", "WATCH": "watch", "NEEDREF": "needref", "SKIP": "skip"}


def write_html(lots: list[Lot], path: Path, target_discount: float) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    rows = []
    for l in lots:
        mv = f"{l.market_value:,.0f}" if l.market_value else "—"
        margin = f"{l.margin_pct:.0f}%" if l.market_value else "—"
        maxb = f"{l.max_bid:,.0f}" if l.max_bid else "—"
        rows.append(
            f'<tr class="{CSS_CLASS[l.verdict]}" data-verdict="{l.verdict}">'
            f"<td>{BADGE[l.verdict]}</td>"
            f'<td class="title"><a href="{html.escape(l.url)}" target="_blank" rel="noopener">{html.escape(l.title) or "—"}</a>'
            f'<div class="meta">{html.escape(l.category)} · {html.escape(l.region)} · {html.escape(l.method)}</div></td>'
            f'<td class="num">{l.start_price:,.0f}</td>'
            f'<td class="num">{mv}</td>'
            f'<td class="num">{margin}</td>'
            f'<td class="num strong">{maxb}</td>'
            f'<td class="num">{l.bids}</td>'
            f'<td class="note">{html.escape(l.note)}</td>'
            f"</tr>"
        )
    counts = {v: sum(1 for l in lots if l.verdict == v) for v in BADGE}
    generated = dt.datetime.now().strftime("%Y-%m-%d %H:%M")

    doc = f"""<!DOCTYPE html>
<html lang="uk"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Лоти Prozorro.Sale — аналіз</title>
<style>
  * {{ box-sizing:border-box; }}
  body {{ font-family:-apple-system,Segoe UI,Roboto,sans-serif; margin:0; background:#0f172a; color:#e2e8f0; }}
  header {{ padding:18px 22px; background:#111c33; border-bottom:1px solid #1e293b; position:sticky; top:0; z-index:5; }}
  h1 {{ margin:0 0 4px; font-size:18px; }}
  .sub {{ color:#94a3b8; font-size:13px; line-height:1.5; }}
  .controls {{ margin-top:12px; display:flex; gap:8px; flex-wrap:wrap; align-items:center; }}
  input[type=search] {{ flex:1; min-width:200px; padding:8px 12px; border-radius:8px; border:1px solid #334155; background:#0b1426; color:#e2e8f0; }}
  button {{ padding:7px 12px; border-radius:8px; border:1px solid #334155; background:#0b1426; color:#e2e8f0; cursor:pointer; }}
  button.active {{ border-color:#6366f1; background:#1e1b4b; }}
  table {{ width:100%; border-collapse:collapse; font-size:13px; }}
  th,td {{ padding:9px 12px; border-bottom:1px solid #1e293b; text-align:left; vertical-align:top; }}
  th {{ position:sticky; top:132px; background:#111c33; cursor:pointer; user-select:none; font-size:12px; color:#cbd5e1; }}
  td.num {{ text-align:right; white-space:nowrap; font-variant-numeric:tabular-nums; }}
  td.strong {{ font-weight:700; color:#a5b4fc; }}
  td.title a {{ color:#e2e8f0; text-decoration:none; }}
  td.title a:hover {{ text-decoration:underline; }}
  .meta {{ color:#64748b; font-size:11px; margin-top:2px; }}
  .note {{ color:#94a3b8; max-width:280px; }}
  tr.good td:first-child {{ border-left:3px solid #16a34a; }}
  tr.watch td:first-child {{ border-left:3px solid #ca8a04; }}
  tr.needref td:first-child {{ border-left:3px solid #475569; }}
  tr.skip td:first-child {{ border-left:3px solid #64748b; }}
  tr.skip, tr.needref {{ opacity:.7; }}
</style></head>
<body>
<header>
  <h1>Лоти Prozorro.Sale — аналіз вигідності</h1>
  <div class="sub">Згенеровано {generated} · всього {len(lots)} ·
  🟢 {counts['GOOD']} варто · 🟡 {counts['WATCH']} стежити · ⚪ {counts['NEEDREF']} без ціни ·
  🔴 {counts['SKIP']} пропустити<br>
  «Макс. ціна» = ринкова вартість мінус запас {target_discount*100:.0f}%. Стовпці клікаються для сортування.</div>
  <div class="controls">
    <input type="search" id="q" placeholder="Пошук за назвою/категорією…">
    <button data-f="ALL" class="active">Всі</button>
    <button data-f="GOOD">🟢 Варто</button>
    <button data-f="WATCH">🟡 Стежити</button>
    <button data-f="NEEDREF">⚪ Треба ціна</button>
    <button data-f="SKIP">🔴 Пропустити</button>
  </div>
</header>
<table id="t">
<thead><tr>
  <th data-k="0">Вердикт</th><th data-k="1">Лот</th>
  <th data-k="2">Старт</th><th data-k="3">Ринок</th>
  <th data-k="4">Маржа</th><th data-k="5">Макс. ціна</th>
  <th data-k="6">Ставки</th><th data-k="7">Коментар</th>
</tr></thead>
<tbody>
{chr(10).join(rows)}
</tbody></table>
<script>
const tb = document.querySelector('#t tbody');
const rows = () => [...tb.querySelectorAll('tr')];
let filter = 'ALL', query = '';
function apply() {{
  rows().forEach(r => {{
    const okF = filter === 'ALL' || r.dataset.verdict === filter;
    const okQ = !query || r.textContent.toLowerCase().includes(query);
    r.style.display = (okF && okQ) ? '' : 'none';
  }});
}}
document.querySelectorAll('button[data-f]').forEach(b => b.onclick = () => {{
  document.querySelectorAll('button[data-f]').forEach(x => x.classList.remove('active'));
  b.classList.add('active'); filter = b.dataset.f; apply();
}});
document.querySelector('#q').oninput = e => {{ query = e.target.value.toLowerCase(); apply(); }};
document.querySelectorAll('th').forEach(th => th.onclick = () => {{
  const k = +th.dataset.k, dir = th.dataset.dir = th.dataset.dir === 'asc' ? 'desc' : 'asc';
  rows().sort((a, b) => {{
    const x = a.children[k].textContent, y = b.children[k].textContent;
    const nx = parseFloat(x.replace(/[^\\d.-]/g, '')), ny = parseFloat(y.replace(/[^\\d.-]/g, ''));
    const cmp = (!isNaN(nx) && !isNaN(ny)) ? nx - ny : x.localeCompare(y, 'uk');
    return dir === 'asc' ? cmp : -cmp;
  }}).forEach(r => tb.appendChild(r));
}});
</script>
</body></html>"""
    path.write_text(doc, encoding="utf-8")


def debug_one() -> None:
    s = session()
    ids = walk_feed(s, max_pages=1, since=None)
    if not ids:
        sys.exit("Стрічка порожня — перевір доступ до мережі.")
    print(json.dumps(fetch_json(s, BASE + DETAIL_PATH.format(id=ids[0])), ensure_ascii=False, indent=2))


def main() -> None:
    p = argparse.ArgumentParser(description="Аналізатор лотів Prozorro.Sale")
    p.add_argument("--max-pages", type=int, default=20, help="скільки сторінок стрічки пройти (100 лотів/стор.)")
    p.add_argument("--since-days", type=int, default=0, help="лише лоти, оновлені за N днів (0 = без обмеження)")
    p.add_argument("--keyword", default="", help="фільтр за словом у назві/категорії (напр. 'авто')")
    p.add_argument("--reference-file", type=Path, help="CSV довідкових цін: keyword,price")
    p.add_argument("--target-discount", type=float, default=0.30,
                   help="бажаний запас знижки від ринку (0.30 = платимо максимум 70%% ринкової ціни)")
    p.add_argument("--workers", type=int, default=8, help="паралельних запитів деталей")
    p.add_argument("--debug-one", action="store_true", help="показати сиру JSON-структуру одного лота і вийти")
    args = p.parse_args()

    if args.debug_one:
        debug_one()
        return

    refs = load_reference(args.reference_file)
    if args.reference_file:
        print(f"Завантажено довідкових цін: {len(refs)}", file=sys.stderr)

    lots = collect(args, refs)
    if not lots:
        print("Активних лотів не знайдено за заданими фільтрами.", file=sys.stderr)
        return

    csv_path, html_path = OUT_DIR / "lots.csv", OUT_DIR / "report.html"
    write_csv(lots, csv_path)
    write_html(lots, html_path, args.target_discount)

    good = sum(1 for l in lots if l.verdict == "GOOD")
    print(f"\nГотово. Лотів: {len(lots)} (🟢 варто: {good}).")
    print(f"  CSV:  {csv_path}")
    print(f"  HTML: {html_path}  ← відкрий у браузері")


if __name__ == "__main__":
    main()
