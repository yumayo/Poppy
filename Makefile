.PHONY: artifact

artifact:
	docker compose run --build --rm build
