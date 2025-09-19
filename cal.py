# pip install msal requests bleak
import msal, requests, json, os, argparse, asyncio, sys
from datetime import datetime, timedelta, timezone
from zoneinfo import ZoneInfo
from typing import List, Dict, Any, Optional

try:
    from bleak import BleakScanner, BleakClient
except ImportError:  # graceful fallback
    BleakScanner = None
    BleakClient = None

BLE_SERVICE_UUID = "7e20c560-55dd-4c7a-9c61-8f6ea7d7c301"
BLE_CHARACTERISTIC_UUID = "9c5a5dd9-3c40-4e58-9d0a-95bf7cb9d302"
DEFAULT_DAYS = 7

# ---------------- O365 Auth & Fetch -----------------

def load_cache(cache_file: str):
    if os.path.exists(cache_file):
        cache = msal.SerializableTokenCache()
        with open(cache_file, "r") as f:
            cache.deserialize(f.read())
        return cache
    return msal.SerializableTokenCache()

def save_cache(cache: msal.SerializableTokenCache, cache_file: str):
    with open(cache_file, "w") as f:
        f.write(cache.serialize())

def acquire_token(client_id: str, tenant_id: str, scopes: List[str], cache_file: str) -> Dict[str, Any]:
    authority = f"https://login.microsoftonline.com/{tenant_id}"
    cache = load_cache(cache_file)
    app = msal.PublicClientApplication(client_id, authority=authority, token_cache=cache)
    accounts = app.get_accounts()
    result = app.acquire_token_silent(scopes, account=accounts[0]) if accounts else None
    if not result or "access_token" not in result:
        flow = app.initiate_device_flow(scopes=scopes)
        if "user_code" not in flow:
            raise RuntimeError("Konnte Device Flow nicht starten.")
        print(flow["message"])  # user instructions
        result = app.acquire_token_by_device_flow(flow)
        if "access_token" not in result:
            raise RuntimeError("Anmeldung fehlgeschlagen: " + str(result))
    save_cache(cache, cache_file)
    return result

def graph_calendar_view(token: str, days: int = DEFAULT_DAYS) -> List[Dict[str, Any]]:
    headers = {"Authorization": f"Bearer {token}"}
    now_berlin = datetime.now(ZoneInfo("Europe/Berlin"))
    # Start gestern 00:00 (wie vorher), Ende + days
    start_berlin = now_berlin.replace(hour=0, minute=0, second=0, microsecond=0) + timedelta(days=-1)
    start_utc = start_berlin.astimezone(timezone.utc)
    end_utc = start_utc + timedelta(days=days)
    start_str = start_utc.strftime('%Y-%m-%dT%H:%M:%SZ')
    end_str = end_utc.strftime('%Y-%m-%dT%H:%M:%SZ')
    url = (
        "https://graph.microsoft.com/v1.0/me/calendarView"
        f"?startDateTime={start_str}&endDateTime={end_str}"
        "&$orderby=start/dateTime&$top=200"
    )
    r = requests.get(url, headers=headers, timeout=30)
    r.raise_for_status()
    return r.json().get("value", [])

# ---------------- Transformation -----------------

def count_attendees(attendees, response_type):
    if response_type == "notResponded":
        return sum(1 for a in attendees if a.get("status", {}).get("response") in ["notResponded", "none"])
    return sum(1 for a in attendees if a.get("status", {}).get("response") == response_type)

def to_local(dt_str: Optional[str]) -> Optional[str]:
    if not dt_str:
        return None
    dt_str = dt_str.split('.')[0]
    dt_utc = datetime.strptime(dt_str, "%Y-%m-%dT%H:%M:%S").replace(tzinfo=timezone.utc)
    dt_local = dt_utc.astimezone(ZoneInfo("Europe/Berlin"))
    return dt_local.strftime("%Y-%m-%dT%H:%M:%S%z")

def condense_events(events: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    out = []
    for evt in events:
        attendees = evt.get("attendees", [])
        organizer = evt.get("organizer", {}).get("emailAddress", {}).get("name") if evt.get("organizer") else None
        location = evt.get("location", {}).get("displayName") if evt.get("location") else None
        if location:
            for phrase in ["Microsoft Teams-Besprechung", "Microsoft Teams Meeting", "Microsoft Teams", "MS Teams"]:
                location = location.replace(phrase, " ")
            location = " ".join(location.split())
        start_local = to_local(evt.get("start", {}).get("dateTime"))
        end_local = to_local(evt.get("end", {}).get("dateTime"))
        date_local = start_local[:10] if start_local else ""
        isRecurring = False
        isMoved = False
        if evt.get("recurrence") or evt.get("seriesMasterId"):
            isRecurring = True
            if evt.get("seriesMasterId") and (evt.get("type") == "exception" or evt.get("originalStart")):
                isMoved = True
        out.append({
            "date": date_local,
            "start": start_local,
            "end": end_local,
            "subject": evt.get("subject"),
            "organizer": organizer,
            "location": location,
            "importance": evt.get("importance"),
            "hasAttachments": evt.get("hasAttachments"),
            "isOnlineMeeting": evt.get("isOnlineMeeting", False),
            "isRecurring": isRecurring,
            "isMoved": isMoved,
            "isCancelled": evt.get("isCancelled", False),
            "numOf": {
                "required": sum(1 for a in attendees if a.get("type") == "required"),
                "optional": sum(1 for a in attendees if a.get("type") == "optional"),
                "accepted": count_attendees(attendees, "accepted"),
                "declined": count_attendees(attendees, "declined"),
                "notResponded": count_attendees(attendees, "notResponded"),
            }
        })
    return out

# ---------------- BLE Send -----------------
async def ble_send(json_path: str, address: Optional[str], chunk_size: int, max_payload: int, debug: bool = False, name_prefix: str = "CalSync", force_resp: bool = False, chunk_delay: float = 0.0, send_time: bool = False, time_only: bool = False):
    if BleakScanner is None or BleakClient is None:
        print("Bleak nicht installiert (pip install bleak)")
        return False
    if not os.path.exists(json_path):
        print(f"Datei {json_path} existiert nicht.")
        return False
    with open(json_path, "r", encoding="utf-8") as f:
        payload = f.read()
    data_bytes = payload.encode("utf-8")
    length = len(data_bytes)
    if length > max_payload:
        print(f"Payload {length} > Limit {max_payload} – Abbruch.")
        return False
    if not address:
        print(f"Scanne nach {name_prefix}...")
        devices = await BleakScanner.discover(timeout=6.0)
        if debug:
            print(f"{len(devices)} Geräte gefunden.")
        matches = []
        for d in devices:
            name = getattr(d, "name", "") or ""
            md = getattr(d, "metadata", None)
            uuids_list = []
            if isinstance(md, dict):
                raw = md.get("uuids") or md.get("service_uuids") or []
                if isinstance(raw, (list, tuple)):
                    uuids_list = [u.lower() for u in raw if isinstance(u, str)]
            cond = name.startswith(name_prefix) or (BLE_SERVICE_UUID.lower() in uuids_list)
            if cond and not address:
                address = getattr(d, 'address', None)
                matches.append((name, address, uuids_list))
        if not address:
            print("Kein passendes Gerät gefunden. Gefundene Geräte:")
            for d in devices:
                print(f"  - {getattr(d,'name','?')} @ {getattr(d,'address','?')}")
            return False
        else:
            for (n,a,u) in matches:
                print(f"Gefunden: {n} @ {a} uuids={u}")
    header_bytes = f"LEN:{length}\n".encode("utf-8")
    # Wenn sehr kleine Payload (<= 40) -> Header + Payload zusammen versuchen
    combined_mode = length <= 40
    print(f"Verbinde zu {address} ... (combined_mode={combined_mode})")
    async with BleakClient(address) as client:
        if not client.is_connected:
            print("Verbindung fehlgeschlagen.")
            return False
        # Zeit vorab senden
        if send_time:
            import time
            epoch = int(time.time())
            time_hdr = f"TIME:{epoch}\n".encode("utf-8")
            if debug: print(f"Sende Zeit: {epoch}")
            await client.write_gatt_char(BLE_CHARACTERISTIC_UUID, time_hdr, response=True)
            if time_only:
                print("Nur Zeit gesendet (--ble-time-only).")
                return True
        if combined_mode:
            packet = header_bytes + data_bytes
            if debug: print(f"Sende kombinierten Frame ({len(packet)} Bytes)")
            await client.write_gatt_char(BLE_CHARACTERISTIC_UUID, packet, response=True)
            print("Übertragung abgeschlossen (kombiniert).")
            return True
        # Normaler Modus: Erst Header
        print("Verbunden. Sende Header...")
        await client.write_gatt_char(BLE_CHARACTERISTIC_UUID, header_bytes, response=True)
        sent = 0
        big_payload = length > 4000
        if big_payload and not force_resp:
            if debug: print("Aktiviere force_resp fuer grosse Payload")
            force_resp = True
        if big_payload and chunk_delay == 0.0:
            chunk_delay = 0.01
        # adapt chunk size conservatively if huge
        if big_payload and chunk_size > 200:
            chunk_size = 200
        while sent < length:
            part = data_bytes[sent:sent+chunk_size]
            await client.write_gatt_char(BLE_CHARACTERISTIC_UUID, part, response=(force_resp or chunk_size < 30))
            sent += len(part)
            if debug:
                print(f"  Chunk gesendet: {sent}/{length}")
            else:
                print(f"  Fortschritt: {sent}/{length} ({sent*100/length:.1f}%)", end='\r')
            if chunk_delay > 0:
                await asyncio.sleep(chunk_delay)
        if not debug:
            print()
        print("Übertragung abgeschlossen.")
    return True

# ---------------- Main -----------------

def build_arg_parser():
    p = argparse.ArgumentParser(description="Fetch O365 calendar and optionally send via BLE to e-paper device.")
    p.add_argument("--client-id", default=os.getenv('O365_CLIENT_ID'), help="Azure AD App Client ID (optional, else prompt)")
    p.add_argument("--tenant-id", default=os.getenv('O365_TENANT_ID'), help="Azure AD Tenant ID (optional, else prompt)")
    p.add_argument("--days", type=int, default=DEFAULT_DAYS, help="Range (days) after yesterday to fetch (default 7)")
    p.add_argument("--output", default="data/calendar-condensed.json", help="Output JSON path")
    p.add_argument("--no-fetch", action="store_true", help="Skip Graph fetch, just BLE send existing file")
    p.add_argument("--ble", action="store_true", help="Send JSON via BLE after (or without) fetch")
    p.add_argument("--ble-address", help="BLE MAC/UUID (skip scan)")
    p.add_argument("--chunk-size", type=int, default=180, help="BLE chunk bytes (<= MTU-3)")
    p.add_argument("--max-payload", type=int, default=60000, help="Abort if JSON longer than this")
    p.add_argument("--ble-debug", action="store_true", help="Verbose BLE Scan/Send Logs")
    p.add_argument("--ble-name", default="CalSync", help="Name prefix to match (default CalSync)")
    p.add_argument("--ble-force-response", action="store_true", help="Send every BLE data chunk with Write Response (slower, more reliable)")
    p.add_argument("--ble-chunk-delay", type=float, default=0.0, help="Sleep seconds between BLE chunks (e.g. 0.02)")
    p.add_argument("--ble-send-time", action="store_true", help="Send current time (epoch UTC) before calendar")
    p.add_argument("--ble-time-only", action="store_true", help="Only send time (no calendar payload)")
    return p

def main():
    args = build_arg_parser().parse_args()
    client_id = args.client_id or (input('Client ID: '))
    tenant_id = args.tenant_id or (input('Tenant ID: '))
    condensed: List[Dict[str, Any]] = []

    if not args.no_fetch:
        print("Authentifiziere & lade Kalender...")
        token_result = acquire_token(client_id, tenant_id, ["https://graph.microsoft.com/.default"], "msal_token_cache.bin")
        events = graph_calendar_view(token_result['access_token'], days=args.days)
        condensed = condense_events(events)
        os.makedirs(os.path.dirname(args.output), exist_ok=True)
        with open(args.output, 'w', encoding='utf-8') as f:
            json.dump(condensed, f, ensure_ascii=False, indent=2)
        print(f"{len(condensed)} Events gespeichert -> {args.output}")
    else:
        print("Graph Fetch übersprungen (--no-fetch). Nutze vorhandene Datei.")
        if not os.path.exists(args.output):
            print(f"Datei {args.output} nicht vorhanden – Abbruch.")
            return 2

    if args.ble:
        print("Starte BLE Übertragung...")
        ok = asyncio.run(ble_send(
            args.output,
            args.ble_address,
            args.chunk_size,
            args.max_payload,
            args.ble_debug,
            args.ble_name,
            args.ble_force_response,
            args.ble_chunk_delay,
            args.ble_send_time,
            args.ble_time_only
        ))
        return 0 if ok else 1
    return 0

if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("Abgebrochen.")
        sys.exit(130)

