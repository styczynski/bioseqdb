import sys
import csv

def check(results_file_path):
    expected_coords = {}
    with open(results_file_path, mode ='r')as file:
        csvFile = csv.reader(file, delimiter='\t')
        expected_coords = {line[0]: (int(line[1]), int(line[2])) for line in csvFile}

    with open("./output.txt", mode='r') as file:
        csvFile = csv.reader(file, delimiter='\t')
        actual_coords = {line[0]: (int(line[1]), int(line[2])) for line in csvFile}

    bad_count = 0
    unmapped_count = 0
    ok_count = 0
    qual_count = dict(AA=0, AB=0, CC=0, DD=0)

    for (seq_id, (expected_s, expected_e)) in expected_coords.items():
        status, message = "BAD", "UNKNOWN"
        if seq_id not in actual_coords:
            unmapped_count += 1
            status, message = "BAD", "UNMAPPED"
        else:
            (actual_s, actual_e) = actual_coords[seq_id]
            diff_start = expected_s - actual_s
            diff_end = expected_e - actual_e

            max_diff = max(abs(diff_start), abs(diff_end))
            sum_diff = abs(diff_start)+abs(diff_end) 

            status = "OK" if max_diff < 20 else "BAD"
            qual = "AA" if sum_diff < 10 else ("AB" if sum_diff < 20 else ("CC" if max_diff < 20 else "DD"))
            message = f"qual={qual} | max_diff={max_diff} sum_diff={sum_diff} | diff=<{diff_start}, {diff_end}>"       

            if status != "OK":
                bad_count += 1
            else:
                ok_count += 1
                qual_count[qual] += 1
        print(f"{seq_id} | status={status} | {message}")

    print(f"\n\n\n=== SUMMARY ===")
    print(f"bad       = {bad_count} ({bad_count / len(expected_coords) * 100}%)")
    print(f"ok        = {ok_count} ({ok_count / len(expected_coords) * 100}%)")
    print(f"unmapped  = {unmapped_count} ({unmapped_count / len(expected_coords) * 100}%)")
    print("\n")
    if ok_count > 0:
        for (key, count) in qual_count.items():
            print(f"ok[{key}]      = {count} ({count/ok_count*100}%)")

if __name__ == '__main__':
    check(sys.argv[1])