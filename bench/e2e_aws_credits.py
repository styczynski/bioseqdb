#!/usr/bin/env python3
"""Read-only, hourly-cached AWS account-plan credits for active fleet accounts.

AWS source: https://docs.aws.amazon.com/aws-cost-management/latest/APIReference/API_freetier_GetAccountPlanState.html
These are AWS-reported account-plan credits, not a real-time spending estimate
or a claim about other promotional-credit pools. Missing data is never zero.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import json
import math
import os
import subprocess
import time

from monitor_e2e_fleet import registered_hosts, REPOSITORY

CACHE = REPOSITORY / 'reporting/bioseq_benchmark_e2e/fleet_controller/aws_credits.json'
ACCOUNTS = {'test2': '090072209895', 'test3': '918106144232',
            'test4': '665651551858', 'test7': '872590157365'}


def fetch(profile):
    row = {'profile': profile, 'checked_at': datetime.now(timezone.utc).isoformat()}
    try:
        result = subprocess.run(['aws', '--profile', profile, '--region', 'us-east-1',
            '--cli-connect-timeout', '5', '--cli-read-timeout', '10',
            'freetier', 'get-account-plan-state', '--output', 'json'],
            capture_output=True, text=True, timeout=20,
            env=os.environ | {'AWS_MAX_ATTEMPTS': '1', 'AWS_PAGER': ''})
        if result.returncode:
            raise RuntimeError(result.stderr.strip()[-400:])
        data = json.loads(result.stdout)
        if data.get('accountId') != ACCOUNTS.get(profile):
            raise ValueError('AWS account identity mismatch or unknown fleet profile')
        balance = data.get('accountPlanRemainingCredits')
        if not isinstance(balance, dict) or balance.get('amount') is None or not balance.get('unit'):
            raise ValueError('AWS did not return remaining account-plan credits')
        amount = float(balance['amount'])
        if not math.isfinite(amount):
            raise ValueError('AWS returned a non-finite credit balance')
        row.update(status='available', account_id=data['accountId'],
                   amount=amount, unit=balance['unit'],
                   plan_status=data.get('accountPlanStatus'), source='freetier.GetAccountPlanState')
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        row.update(status='unavailable', reason=str(exc))
    return row


def credit_snapshot(*, refresh=False):
    profiles = sorted({host[0].split('-', 1)[0] for host in registered_hosts()})
    try:
        cached = json.loads(CACHE.read_bytes())
        if not refresh and 0 <= time.time() - cached['timestamp'] < 3600 and cached['profiles'] == profiles:
            return cached
    except (OSError, ValueError, KeyError):
        pass
    with ThreadPoolExecutor(max_workers=4) as pool:
        rows = list(pool.map(fetch, profiles))
    data = {'timestamp': time.time(), 'profiles': profiles, 'accounts': rows}
    CACHE.parent.mkdir(parents=True, exist_ok=True)
    # Unique temp names allow concurrent manual and hourly status requests.
    temporary = CACHE.with_name(CACHE.name + '.' + str(os.getpid()) + '.tmp')
    temporary.write_text(json.dumps(data, indent=2) + '\n')
    temporary.replace(CACHE)
    return data


def format_credit_balances(*, refresh=False):
    data = credit_snapshot(refresh=refresh)
    lines = ['AWS-reported remaining account-plan credits (hourly cached; not a real-time balance):',
             '', '| Profile | Account | Credits remaining | Checked (UTC) |',
             '| :-- | :-- | --: | :-- |']
    reasons = []
    for row in data['accounts']:
        balance = f"{row['amount']:.2f} {row['unit']}" if row['status'] == 'available' else 'Unavailable'
        lines.append(f"| {row['profile']} | {row.get('account_id', ACCOUNTS.get(row['profile'], 'unknown'))} | {balance} | {row['checked_at']} |")
        if row['status'] != 'available':
            reasons.append(row['profile'] + ': ' + row['reason'].replace('\n', ' '))
    if reasons:
        lines.extend(['', *reasons])
    return '\n'.join(lines)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--refresh', action='store_true')
    parser.add_argument('--log', action='store_true', help='Append tuple counters and credits to DOC0432')
    args = parser.parse_args()
    output = format_credit_balances(refresh=args.refresh)
    if args.log:
        from e2e_tuple_progress import append_progress_log
        print(append_progress_log(title='tuple coverage and AWS credits'))
    print(output)
