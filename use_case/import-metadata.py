#/usr/bin/env python3

import sys
import os
import csv
import re

import psycopg2

def is_valid_date(date):
    # reject inexact dates (2020, 2020-07)
    return date is not None and re.search('^\d{4}-\d{2}-\d{2}$', date) is not None

def unify(row):
    row['strain'] = row['strain']\
        .replace('PuertoRico', 'Puerto Rico')\
        .replace('NorthernIreland', 'Northern Ireland')\
        .replace('CzechRepublic', 'Czech Republic')\
        .replace('UnitedArabEmirates', 'United Arab Emirates')\
        .replace('HongKong', 'Hong Kong')\
        .replace('SriLanka', 'Sri_Lanka')\
        .replace('SouthAfrica', 'South_Africa')\
        .replace('MOH', '_MOH')\
        .replace('USA/PR', 'Puerto Rico/PR')
    if row['date_submitted'] == '2020-10-19':
        row['strain'] = row['strain']\
            .replace('Czech Republic', 'Czech_Republic')\
            .replace('Northern Ireland', 'Northern_Ireland')
    if row['date_submitted'] == '2020-10-29':
        row['strain'] = row['strain']\
            .replace('Hong Kong', 'Hong_Kong')\

    row['submitted_date'] = row['date_submitted']

    # undefined data => None <=> null in sql
    for k, v in row.items():
        if v == '' or v == 'unknown' or v == '?':
            row[k] = None
    # there are entries in dataset with age of babies (in months)
    if row['age'] is not None and row['age'].isnumeric():
        row['age'] += ' years'

    if not is_valid_date(row['submitted_date']):
        row['submitted_date'] = None

    if not is_valid_date(row['retrieval_date']):
        row['retrieval_date'] = None

    # ignore age ranges (60 - 65, > 18, < 18)
    if row['age'] is not None and ('-' in row['age'] or '<' in row['age'] or '>' in row['age']):
        row['age'] = None

    return row

def parse_metadata(file):
    with open(sys.argv[1], 'r') as csvfile:
        reader = csv.DictReader(csvfile)
        for row in reader:
            yield row

if len(sys.argv) != 2:
    print('usage: ./import.py <meta-csv>')
    sys.exit(1)

credentials = {
    'dbname' : os.getenv('DB_NAME'),
    'user' : os.getenv('DB_USER'),
    'password' : os.getenv('DB_PASS'),
    'host' : os.getenv('DB_HOST'),
    'port' : os.getenv('DB_PORT'),
}

with psycopg2.connect(**credentials) as conn:
    cur = conn.cursor()
    for row in map(unify, parse_metadata(sys.argv[1])):
        cur.execute("""UPDATE dataset
                SET
                    retrieval_date = %(retrieval_date)s,
                    submitted_date = %(submitted_date)s,
                    region = %(region)s,
                    country = %(country)s,
                    division = %(division)s,
                    location = %(location)s,
                    age = %(age)s,
                    sex = %(sex)s,
                    lineage = %(lineage)s
                WHERE strain = %(strain)s;
                """, row)
        if cur.rowcount != 1:
            conn.rollback()
            raise Exception(repr(row))
    conn.commit()
    cur.close()
