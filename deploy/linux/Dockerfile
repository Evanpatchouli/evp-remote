FROM node:22-bookworm-slim
WORKDIR /srv/evp
COPY package.json package-lock.json ./
RUN npm ci --omit=dev --ignore-scripts --no-audit --no-fund
COPY server/ server/
USER node
CMD ["node", "server/main.mjs"]
