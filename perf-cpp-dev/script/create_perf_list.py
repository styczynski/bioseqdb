import re
import subprocess
import os


codes_regex = re.compile(r'Codes\s+:\s?([a-zA-Z0-9]+)')

def extract_events(content: str):
    # Find the event name
    name_match = re.search(r"Name\s+:\s+([a-zA-Z0-9\_\-]+)", content)
    if not name_match:
        return []
    event_name = name_match.group(1)

    # Find all Umask entries and their descriptions
    umask_matches = re.findall(r"Umask-\d+\s+:\s+0x[0-9a-fA-F]+\s+:\s+PMU\s+:\s+\[(.*?)\]", content)

    if not umask_matches:
        return [event_name]

    # Combine the event name with each Umask
    return [f"{event_name}.{umask.replace(' ', '_')}" for umask in umask_matches]

## Clone or pull the repository
if not os.path.exists('libpfm4'):
    subprocess.run(['git', 'clone', '-b', 'master', '--single-branch', 'https://github.com/wcohen/libpfm4.git', 'libpfm4'], stdout=subprocess.PIPE)
else:
    os.chdir('libpfm4')
    subprocess.run(['git',  'pull'], stdout=subprocess.PIPE)
    os.chdir('..')

## Make the libpfm4 lib
os.chdir('libpfm4')
subprocess.run(['make'], stdout=subprocess.PIPE)

## Read all the event infos
events_result = subprocess.run(['examples/showevtinfo'], stdout=subprocess.PIPE)
counters = []

## Transform into counters
for events_content in str(events_result.stdout).split('#-----------------------------'):
    events = extract_events(events_content)
    for event_to_check in events:
        event_result = subprocess.run([f'examples/check_events', event_to_check], stdout=subprocess.PIPE)
        codes_match = re.search(r"Codes\s+:\s+(0x[0-9a-fA-F]+)", str(event_result.stdout))

        if codes_match:
            counters.append((event_to_check, codes_match.group(1)))

with open('../perf_list.csv', 'w') as perf_out_file:
    for counter in counters:
        perf_out_file.write(f'{counter[0]},{counter[1]}\n')

print('-------------------------------------------------------------------------------------------------------------------------------------------------')
print('WARNING: The \'perf-list\' target is deprecated and will be removed with perf-cpp v0.14! Use the processor-specific event files in events/ instead.')
print('-------------------------------------------------------------------------------------------------------------------------------------------------')
print(f'Wrote {len(counters)} counter definitions to \'perf_list.csv\'.')

