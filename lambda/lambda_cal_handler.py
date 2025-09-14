import msal, requests, json, os
from zoneinfo import ZoneInfo
from datetime import datetime, timedelta, timezone

def handler(event, context):
    TENANT_ID = os.environ["TENANT_ID"]
    CLIENT_ID = os.environ["CLIENT_ID"]
    CLIENT_SECRET = os.environ["CLIENT_SECRET"]
    AUTHORITY = f"https://login.microsoftonline.com/{TENANT_ID}"
    SCOPES = ["https://graph.microsoft.com/.default"]

    app = msal.ConfidentialClientApplication(
        CLIENT_ID,
        authority=AUTHORITY,
        client_credential=CLIENT_SECRET
    )
    result = app.acquire_token_for_client(scopes=SCOPES)
    if "access_token" not in result:
        raise RuntimeError(f"Token error: {result.get('error_description', result)}")

    headers = {"Authorization": f"Bearer {result['access_token']}"}

    now_berlin = datetime.now(ZoneInfo("Europe/Berlin"))
    start_berlin = now_berlin.replace(hour=0, minute=0, second=0, microsecond=0)
    start_utc = start_berlin.astimezone(timezone.utc) + timedelta(days=-1)
    end_utc = start_utc + timedelta(days=7)
    start_str = start_utc.strftime('%Y-%m-%dT%H:%M:%SZ')
    end_str = end_utc.strftime('%Y-%m-%dT%H:%M:%SZ')
    USER_ID = os.environ["USER_ID"]
    url = (
        f"https://graph.microsoft.com/v1.0/users/{USER_ID}/calendarView"
        f"?startDateTime={start_str}&endDateTime={end_str}"
        "&$orderby=start/dateTime&$top=100"
    )

    resp = requests.get(url, headers=headers)
    resp.raise_for_status()
    raw = resp.json()
    data = raw.get("value", [])

    def count_attendees(attendees, response_type):
        if response_type == "notResponded":
            return sum(1 for a in attendees if a.get("status", {}).get("response") in ["notResponded", "none"])
        return sum(1 for a in attendees if a.get("status", {}).get("response") == response_type)

    def to_local(dt_str):
        if not dt_str:
            return None
        dt_str = dt_str.split('.')[0]
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
        isRecurring = False
        isMoved = False
        if evt.get("recurrence") or evt.get("seriesMasterId"):
            isRecurring = True
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

    return {
        "statusCode": 200,
        "headers": {"Content-Type": "application/json"},
        "body": json.dumps(condensed, ensure_ascii=False, indent=2)
    }
