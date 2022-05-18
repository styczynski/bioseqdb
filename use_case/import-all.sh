#/bin/bash
set -euo pipefail

DATASET_PATH=${DATASET_PATH:-data}

export DB_NAME=${DB_NAME:-bioseqdb-testing}
export DB_USER=${DB_USER:-postgres}
export DB_PASS=${DB_PASS:-}
export DB_PORT=${DB_PORT:-5432}
export DB_HOST=${DB_HOST:-localhost}

export DB_URI="postgresql://${DB_USER}:${DB_PASS}@${DB_HOST}:${DB_PORT}/${DB_NAME}"

psql "${DB_URI}" -f up.sql

for fasta in "${DATASET_PATH}/"*.fasta; do
  meta="${fasta%.fasta}.meta.csv"

  # Adjust sequence header to match import tool's format.
  sed -i 's/|.*//' "${fasta}"
  # Pass correct filenames to real import tool.
  bioseqdb_import dataset strain seq "${fasta}"
  # Add all metadata to enties.
  python import-metadata.py "${meta}"
  echo "imported ${fasta} ${meta}"
done
