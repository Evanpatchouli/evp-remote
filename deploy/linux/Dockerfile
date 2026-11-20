FROM --platform=$BUILDPLATFORM node:22-bookworm-slim AS dependencies
WORKDIR /srv/evp
COPY package.json package-lock.json ./
# The locked production dependencies are JavaScript; no native install scripts are used.
RUN npm ci --omit=dev --ignore-scripts --no-audit --no-fund
COPY LICENSE deploy/linux/compose.yaml deploy/linux/configure.sh deploy/
ARG EVP_SIGNAL_IMAGE
RUN if [ -n "$EVP_SIGNAL_IMAGE" ]; then \
      sed -i "/^  signal:$/,/^  [^ ]/s|^    image: .*|    image: $EVP_SIGNAL_IMAGE|" deploy/compose.yaml && \
      grep -Fxq "    image: $EVP_SIGNAL_IMAGE" deploy/compose.yaml; \
    fi

FROM node:22-bookworm-slim
WORKDIR /srv/evp
COPY --from=dependencies /srv/evp/node_modules/ node_modules/
COPY server/ server/
COPY --from=dependencies /srv/evp/deploy/ /usr/share/evp-remote/deploy/
USER node
CMD ["node", "server/main.mjs"]
