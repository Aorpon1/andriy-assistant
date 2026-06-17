#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Маленький парсер аукціонів UUB (sale.uub.com.ua).

Що вміє:
  1. Завантажити сторінку лота за посиланням.
  2. Витягнути дані лота (назва, ціна, рік, пробіг, опис, фото) —
     пробує кілька способів, тому стійкий до змін верстки сайту.
  3. Зробити простий аналіз: вигідно купувати чи ні.
  4. Дати посилання для подвійної перевірки ринкової ціни (AUTO.RIA, OLX).

Запуск:
    pip install requests beautifulsoup4
    python uub_parser.py "https://sale.uub.com.ua/auction/ALE001-UA-..."
    python uub_parser.py "<url>" --market 350000      # якщо знаєш ринкову ціну
"""

import sys
import re
import json
import argparse

import requests
from bs4 import BeautifulSoup

HEADERS = {
    "User-Agent": (
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/120.0 Safari/537.36"
    ),
    "Accept-Language": "uk,en;q=0.9",
    "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
}


# ----------------------------------------------------------------------
# 1. Завантаження сторінки
# ----------------------------------------------------------------------
def fetch(url):
    """Завантажує HTML сторінки лота."""
    resp = requests.get(url, headers=HEADERS, timeout=30)
    resp.raise_for_status()
    resp.encoding = resp.apparent_encoding or "utf-8"
    return resp.text


# ----------------------------------------------------------------------
# 2. Витягування даних лота (кілька стратегій)
# ----------------------------------------------------------------------
def _price_to_number(text):
    """'350 000 грн' / '350,000.00' -> 350000.0 (або None)."""
    if text is None:
        return None
    s = str(text)
    # лишаємо тільки цифри, крапки, коми, пробіли
    s = re.sub(r"[^\d.,\s]", "", s).strip()
    s = s.replace(" ", " ").replace(" ", "")
    if not s:
        return None
    # якщо є і кома, і крапка — кома це роздільник тисяч
    if "," in s and "." in s:
        s = s.replace(",", "")
    else:
        s = s.replace(",", ".")
    try:
        return float(s)
    except ValueError:
        return None


def _find_prices_in_text(text):
    """Знаходить усі числа біля слів 'грн/UAH/ціна' у довільному тексті."""
    found = []
    for m in re.finditer(r"(\d[\d\s .,]{2,})\s*(?:грн|uah|₴)", text, re.I):
        n = _price_to_number(m.group(1))
        if n:
            found.append(n)
    return found


def _from_json_ld(soup):
    """Структуровані дані Schema.org (Product/Offer) — найнадійніше."""
    data = {}
    for tag in soup.find_all("script", type="application/ld+json"):
        try:
            obj = json.loads(tag.string or "{}")
        except (json.JSONDecodeError, TypeError):
            continue
        for node in obj if isinstance(obj, list) else [obj]:
            if not isinstance(node, dict):
                continue
            if node.get("name") and not data.get("title"):
                data["title"] = node["name"]
            if node.get("description") and not data.get("description"):
                data["description"] = node["description"]
            offer = node.get("offers") or {}
            if isinstance(offer, list):
                offer = offer[0] if offer else {}
            if isinstance(offer, dict) and offer.get("price"):
                data.setdefault("price", _price_to_number(offer["price"]))
                data.setdefault("currency", offer.get("priceCurrency"))
    return data


def _from_embedded_json(html):
    """Дані з window.__NEXT_DATA__ / __NUXT__ / initialState тощо."""
    out = {}
    for pat in (
        r"__NEXT_DATA__\s*=\s*(\{.*?\})\s*</script>",
        r"window\.__NUXT__\s*=\s*(\{.*?\});",
        r"window\.__INITIAL_STATE__\s*=\s*(\{.*?\});",
    ):
        m = re.search(pat, html, re.S)
        if not m:
            continue
        try:
            blob = json.dumps(json.loads(m.group(1)), ensure_ascii=False)
        except json.JSONDecodeError:
            continue
        prices = _find_prices_in_text(blob)
        if prices:
            out["price"] = min(prices)  # стартова зазвичай найменша
        break
    return out


def _from_html_heuristics(soup):
    """Запасний варіант: шукаємо по тегах і ключових словах."""
    data = {}
    if soup.title and soup.title.string:
        data["title"] = soup.title.string.strip()
    h1 = soup.find(["h1", "h2"])
    if h1:
        data["title"] = h1.get_text(strip=True)

    text = soup.get_text(" ", strip=True)
    prices = _find_prices_in_text(text)
    if prices:
        data["price"] = min(prices)
        data["price_candidates"] = sorted(set(prices))
    return data


# характеристики авто з тексту
RE_YEAR = re.compile(r"\b(19[89]\d|20[0-3]\d)\b")
RE_MILEAGE = re.compile(r"(\d[\d\s .,]{1,})\s*(?:тис\.?\s*км|км|km)", re.I)
RE_VOLUME = re.compile(r"(\d[.,]\d)\s*(?:л|л\.|liter)", re.I)


def _car_attrs(text):
    attrs = {}
    y = RE_YEAR.search(text)
    if y:
        attrs["year"] = int(y.group(1))
    m = RE_MILEAGE.search(text)
    if m:
        km = _price_to_number(m.group(1))
        if km and "тис" in m.group(0).lower() and km < 1000:
            km *= 1000
        attrs["mileage_km"] = km
    v = RE_VOLUME.search(text)
    if v:
        attrs["engine_l"] = _price_to_number(v.group(1))
    return attrs


def extract_lot(html, url):
    """Збирає все докупи: пробує стратегії від надійних до запасних."""
    soup = BeautifulSoup(html, "html.parser")
    lot = {"url": url}

    # id лота з URL: ALE001-UA-20260611-19752
    m = re.search(r"/auction/([A-Z0-9-]+)", url)
    if m:
        lot["lot_id"] = m.group(1)

    for source in (_from_json_ld(soup), _from_embedded_json(html), _from_html_heuristics(soup)):
        for k, v in source.items():
            if v and not lot.get(k):
                lot[k] = v

    lot.update(_car_attrs(soup.get_text(" ", strip=True)))
    return lot


# ----------------------------------------------------------------------
# 3. Аналіз вигідності
# ----------------------------------------------------------------------
def analyze(lot, market_price=None):
    """Простий аналіз: порівнює ціну лота з ринковою."""
    price = lot.get("price")
    notes = []
    verdict = "❓ Недостатньо даних"

    if price is None:
        notes.append("Не вдалося прочитати ціну — перевір вручну на сторінці.")
        return {"verdict": verdict, "notes": notes}

    notes.append(f"Ціна лота: {price:,.0f} грн".replace(",", " "))

    if market_price:
        diff = market_price - price
        pct = diff / market_price * 100
        notes.append(f"Ринкова ціна (введена): {market_price:,.0f} грн".replace(",", " "))
        notes.append(f"Різниця: {diff:,.0f} грн ({pct:+.0f}%)".replace(",", " "))
        if pct >= 25:
            verdict = "✅ ВИГІДНО — суттєво нижче ринку"
        elif pct >= 10:
            verdict = "🟢 Непогано — трохи дешевше ринку"
        elif pct >= -5:
            verdict = "🟡 По ринку — без вигоди"
        else:
            verdict = "🔴 Дорого — вище ринку, не варто"
    else:
        notes.append(
            "Ринкову ціну не задано. Запусти з --market <ціна> "
            "або звір по посиланнях нижче."
        )

    # ризики авто
    if lot.get("year") and lot["year"] < 2008:
        notes.append("⚠️ Старе авто (рік < 2008) — закладай ремонт.")
    if lot.get("mileage_km") and lot["mileage_km"] > 250000:
        notes.append("⚠️ Великий пробіг (>250 тис. км).")
    notes.append("⚠️ Аукціонне авто часто без права повернення — перевір опис і документи.")

    return {"verdict": verdict, "notes": notes}


# ----------------------------------------------------------------------
# 4. Подвійна перевірка ринкової ціни
# ----------------------------------------------------------------------
def market_links(lot):
    """Готує посилання для звірки ціни."""
    q_parts = []
    title = lot.get("title", "")
    # беремо перші 3-4 слова назви як пошуковий запит
    words = re.findall(r"[A-Za-zА-Яа-яІіЇїЄєҐґ0-9]+", title)[:4]
    if lot.get("year"):
        words.append(str(lot["year"]))
    q = " ".join(words) or "авто"
    from urllib.parse import quote_plus
    qe = quote_plus(q)
    return {
        "query": q,
        "AUTO.RIA": f"https://auto.ria.com/uk/search/?q={qe}",
        "OLX": f"https://www.olx.ua/uk/list/q-{quote_plus(q.replace(' ', '-'))}/",
        "Google": f"https://www.google.com/search?q={qe}+ціна",
    }


# ----------------------------------------------------------------------
# CLI
# ----------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="Парсер аукціонів UUB")
    ap.add_argument("url", help="посилання на лот")
    ap.add_argument("--market", type=float, default=None,
                    help="ринкова ціна (грн) для порівняння")
    ap.add_argument("--json", action="store_true", help="вивести як JSON")
    args = ap.parse_args()

    try:
        html = fetch(args.url)
    except Exception as e:
        print(f"❌ Не вдалося завантажити сторінку: {e}")
        sys.exit(1)

    lot = extract_lot(html, args.url)
    result = analyze(lot, args.market)
    links = market_links(lot)

    if args.json:
        print(json.dumps({"lot": lot, "analysis": result, "market": links},
                         ensure_ascii=False, indent=2))
        return

    print("=" * 50)
    print("ЛОТ:", lot.get("title", "—"))
    print("ID :", lot.get("lot_id", "—"))
    if lot.get("year"):
        print("Рік:", lot["year"])
    if lot.get("mileage_km"):
        print("Пробіг:", f"{lot['mileage_km']:,.0f} км".replace(",", " "))
    print("-" * 50)
    print("ВИСНОВОК:", result["verdict"])
    for n in result["notes"]:
        print("  •", n)
    print("-" * 50)
    print("ПЕРЕВІР РИНКОВУ ЦІНУ (запит:", links["query"] + "):")
    for name in ("AUTO.RIA", "OLX", "Google"):
        print(f"  {name}: {links[name]}")
    print("=" * 50)


if __name__ == "__main__":
    main()
