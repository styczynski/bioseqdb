.PHONY: deps serve build gh_deploy

serve:
	mkdocs serve

deps:
	pip install -r requirements.txt

build:
	mkdocs build

gh_deploy:
	mkdocs gh-deploy --force --strict
