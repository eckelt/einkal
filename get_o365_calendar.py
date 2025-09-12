# Fetches calendar events from Office 365 using Microsoft Graph API
# Usage: python get_o365_calendar.py

import os
import requests
from datetime import datetime, timedelta, timezone

# Get credentials from environment variables or prompt
CLIENT_ID = os.getenv('O365_CLIENT_ID') or input('Client ID: ')
TENANT_ID = os.getenv('O365_TENANT_ID') or input('Tenant ID: ')
CLIENT_SECRET = os.getenv('O365_CLIENT_SECRET') or input('Client Secret: ')

# OAuth2 token endpoint
TOKEN_URL = f'https://login.microsoftonline.com/{TENANT_ID}/oauth2/v2.0/token'

# Request access token
payload = {
    'client_id': CLIENT_ID,
    'scope': 'https://graph.microsoft.com/.default',
    'client_secret': CLIENT_SECRET,
    'grant_type': 'client_credentials'
}
response = requests.post(TOKEN_URL, data=payload)
response.raise_for_status()
token = response.json()['access_token']


USER_EMAIL = os.getenv('O365_USER_EMAIL') or input('User Email (UPN): ')

# Get calendar events for the next 7 days
headers = {'Authorization': f'Bearer {token}'}
start = datetime.now(timezone.utc).isoformat()
end = (datetime.now(timezone.utc) + timedelta(days=7)).isoformat()
url = f'https://graph.microsoft.com/v1.0/users/{USER_EMAIL}/calendar/events?$filter=start/dateTime ge \'{start}\' and end/dateTime le \'{end}\'&$orderby=start/dateTime'

resp = requests.get(url, headers=headers)
resp.raise_for_status()
events = resp.json().get('value', [])

if not events:
    print('No events found.')
else:
    for event in events:
        print(f"{event['subject']} | {event['start']['dateTime']} - {event['end']['dateTime']}")
