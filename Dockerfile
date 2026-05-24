FROM python:3.11-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY nas_status_server.py .
COPY push_event.py .
COPY bridge_poll.py .
COPY run_services.py .
COPY bridge_config.example.json ./bridge_config.json
COPY examples ./examples

RUN mkdir -p /data /app/data

ENV HOST=0.0.0.0
ENV PORT=8099
ENV DISK_PATH=/
ENV EVENT_STORE_PATH=/data/latest_event.json
ENV BRIDGE_CONFIG=/app/bridge_config.json
ENV RUN_SERVER=1
ENV RUN_BRIDGE=1
ENV PYTHONUNBUFFERED=1

EXPOSE 8099

CMD ["python", "run_services.py"]
