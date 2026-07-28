
import csv
with open('./data_big/reads20Ma.txt', mode ='r')as file:
  csvFile = csv.reader(file, delimiter='\t')
  expected_coords = {line[0]: (line[1], line[2]) for line in csvFile}
  print(expected_coords)