FROM python:3.11-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY nas_status_server.py .
COPY push_event.py .
COPY bridge_poll.py .
COPY bridge_config.example.json ./bridge_config.json
COPY examples ./examples

RUN mkdir -p /data

ENV HOST=0.0.0.0
ENV PORT=8099
ENV DISK_PATH=/
ENV EVENT_STORE_PATH=/data/latest_event.json

EXPOSE 8099

CMD ["python", "nas_status_server.py"]
