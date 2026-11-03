# Build from repository root, with an owner-reviewed immutable Google CLI image.
ARG GOOGLE_CLI_IMAGE
FROM ${GOOGLE_CLI_IMAGE}
WORKDIR /app
COPY scripts/gcp_spot_runner.py scripts/gcp_spot_cloud.py scripts/gcp_spot_recovery.py scripts/gcp_spot_checkpoint.py scripts/gcp_spot_janitor.py ./
ENTRYPOINT ["python3", "/app/gcp_spot_janitor.py"]
