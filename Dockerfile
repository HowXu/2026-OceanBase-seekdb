FROM python:3.12.10-slim AS builder
RUN apt-get update && apt-get install -y --no-install-recommends cmake g++ make && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY minivectordb/requirements.txt /app/minivectordb/requirements.txt
RUN python -m venv /opt/venv && /opt/venv/bin/python -m pip install --no-cache-dir -r minivectordb/requirements.txt
COPY minivectordb/CMakeLists.txt /app/minivectordb/CMakeLists.txt
COPY minivectordb/cpp /app/minivectordb/cpp
RUN cmake -S minivectordb -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    -DMINIVECTORDB_ENABLE_FAULT_INJECTION=OFF \
    -DPython_EXECUTABLE=/opt/venv/bin/python \
    -Dpybind11_DIR="$(/opt/venv/bin/python -m pybind11 --cmakedir)" && cmake --build build -j2

FROM python:3.12.10-slim AS runtime
WORKDIR /app
COPY --from=builder /opt/venv /opt/venv
COPY --from=builder /app/build/python /app/lib
COPY minivectordb/python /app/python
RUN mkdir /data
ENV PYTHONPATH=/app/lib:/app/python PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1 TMPDIR=/data
ENTRYPOINT ["/opt/venv/bin/python", "-m", "minivectordb"]
