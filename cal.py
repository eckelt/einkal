# pip install msal requests
import msal, requests, json, os
from time import sleep
from zoneinfo import ZoneInfo
from datetime import datetime, timedelta, timezone

CLIENT_ID = os.getenv('O365_CLIENT_ID') or input('Client ID: ')
TENANT_ID = os.getenv('O365_TENANT_ID') or input('Tenant ID: ')

AUTHORITY = f"https://login.microsoftonline.com/{TENANT_ID}"
SCOPES = ["https://graph.microsoft.com/.default"]  # delegated scopes


# Persistent token cache
cache_file = "msal_token_cache.bin"
if os.path.exists(cache_file):
    cache = msal.SerializableTokenCache()
    with open(cache_file, "r") as f:
        cache.deserialize(f.read())
else:
    cache = msal.SerializableTokenCache()

# Public client (ohne Secret)
app = msal.PublicClientApplication(CLIENT_ID, authority=AUTHORITY, token_cache=cache)


# Try to get token silently from cache
accounts = app.get_accounts()
if accounts:
    result = app.acquire_token_silent(SCOPES, account=accounts[0])
else:
    result = None
if not result or "access_token" not in result:
    # Device Code Flow starten
    flow = app.initiate_device_flow(scopes=SCOPES)
    if "user_code" not in flow:
        raise RuntimeError("Konnte Device Flow nicht starten.")
    print(flow["message"])  # "Auf https://microsoft.com/devicelogin gehen und Code eingeben ..."
    result = app.acquire_token_by_device_flow(flow)  # hier gibst du den Consent
    if "access_token" not in result:
        raise RuntimeError("Anmeldung fehlgeschlagen: " + str(result))
    # Save cache after authentication
    with open(cache_file, "w") as f:
        f.write(cache.serialize())
else:
    # Save cache after silent token acquisition (refresh)
    with open(cache_file, "w") as f:
        f.write(cache.serialize())

headers = {"Authorization": f"Bearer {result['access_token']}"}

# Nächste 7 Tage über calendarView abrufen (empfohlen)


# Startzeit: heute 00:00 Uhr Europe/Berlin, als UTC für Graph API
from zoneinfo import ZoneInfo
now_berlin = datetime.now(ZoneInfo("Europe/Berlin"))
start_berlin = now_berlin.replace(hour=0, minute=0, second=0, microsecond=0)
start_utc = start_berlin.astimezone(timezone.utc) + timedelta(days=-1)
end_utc = start_utc + timedelta(days=7)
start_str = start_utc.strftime('%Y-%m-%dT%H:%M:%SZ')
end_str = end_utc.strftime('%Y-%m-%dT%H:%M:%SZ')
url = (
    "https://graph.microsoft.com/v1.0/me/calendarView"
    f"?startDateTime={start_str}&endDateTime={end_str}"
    "&$orderby=start/dateTime&$top=100"
)

resp = requests.get(url, headers=headers)
resp.raise_for_status()
raw = resp.json()
data = raw.get("value", [])


def count_attendees(attendees, response_type):
    # 'notResponded' zählt auch 'none'
    if response_type == "notResponded":
        return sum(1 for a in attendees if a.get("status", {}).get("response") in ["notResponded", "none"])
    return sum(1 for a in attendees if a.get("status", {}).get("response") == response_type)

def to_local(dt_str):
    # Microsoft Graph liefert z.B. '2025-09-11T11:00:00.0000000'
    if not dt_str:
        return None
    dt_str = dt_str.split('.')[0]  # Mikrosekunden entfernen
    dt_utc = datetime.strptime(dt_str, "%Y-%m-%dT%H:%M:%S")
    dt_utc = dt_utc.replace(tzinfo=timezone.utc)
    dt_local = dt_utc.astimezone(ZoneInfo("Europe/Berlin"))
    return dt_local.strftime("%Y-%m-%dT%H:%M:%S%z")

condensed = []
for evt in data:
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
    # Neue Logik: Serien- und verschobene Termine erkennen
    isRecurring = False
    isMoved = False
    if evt.get("recurrence") or evt.get("seriesMasterId"):
        isRecurring = True
        # moved: Ausnahme-Instanz (Exception) oder originalStart vorhanden
        if evt.get("seriesMasterId") and (
            evt.get("type") == "exception" or evt.get("originalStart")
        ):
            isMoved = True
    entry = {
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
        "numOf": {
            "required": sum(1 for a in attendees if a.get("type") == "required"),
            "optional": sum(1 for a in attendees if a.get("type") == "optional"),
            "accepted": count_attendees(attendees, "accepted"),
            "declined": count_attendees(attendees, "declined"),
            "notResponded": count_attendees(attendees, "notResponded"),
        }
    }
    condensed.append(entry)

with open("data/calendar-condensed.json", "w", encoding="utf-8") as f:
    json.dump(condensed, f, ensure_ascii=False, indent=2)

for evt in condensed:
    print(f"{evt['date']} | {evt['start']} – {evt['end']} | {evt['subject']}")

 