FROM python:3.12-slim
WORKDIR /app
COPY pyproject.toml ./
COPY ue_sim ./ue_sim
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1
RUN pip install --no-cache-dir . && useradd --uid 10001 --no-create-home sim
USER 10001
ENTRYPOINT ["python", "-m", "ue_sim"]
CMD ["live", "--help"]
