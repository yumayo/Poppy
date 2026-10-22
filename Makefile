.PHONY: dev artifact

artifact: dev
	docker compose exec -T dev /usr/local/bin/poppy-build

dev:
	docker compose up -d --build dev
