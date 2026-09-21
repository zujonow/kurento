#!/usr/bin/env bash
set -euo pipefail

# Push a locally built kurento-media-server image to ECR.
#
# Usage: ./pushHelper.sh <local_tag> [remote_tag]
#   <local_tag>   tag of the local image (kurento-media-server:<local_tag>)
#   [remote_tag]  tag to push to ECR (defaults to <local_tag>)
#
# Examples:
#   ./pushHelper.sh slim                 # push local :slim as :slim
#   ./pushHelper.sh slim dev-slim-0.0.1  # push local :slim as :dev-slim-0.0.1
#
# Note: you must be logged in to ECR first (uncomment the line below or run it manually).

if [ "$#" -lt 1 ]; then
  echo "Usage: $0 <local_tag> [remote_tag]"
  exit 1
fi

# aws ecr get-login-password --region ap-south-1 --profile ecr-manager \
#   | docker login --username AWS --password-stdin 533352480343.dkr.ecr.ap-south-1.amazonaws.com

docker tag "kurento-media-server:$1" "533352480343.dkr.ecr.ap-south-1.amazonaws.com/kurento-media-server:${2:-$1}"
docker push "533352480343.dkr.ecr.ap-south-1.amazonaws.com/kurento-media-server:${2:-$1}"
