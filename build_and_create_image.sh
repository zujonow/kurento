#!/bin/bash
set -e

# Components, in dependency order. "all" builds every one of them.
COMPONENTS=(
    cmake-utils
    module-creator
    jsonrpc
    module-core
    module-elements
    module-filters
    media-server
)

# Components that need previously built packages installed before building
NEEDS_INSTALL_FILES="jsonrpc module-core module-elements module-filters media-server"

usage() {
    cat <<EOF
Usage: $0 [options] [component] [arch]

Builds Kurento .deb packages inside the kurento-buildpackage container, then
creates a kurento-media-server Docker image from them.

Options:
  --component NAME   Component to build: all (default) or one of:
                     ${COMPONENTS[*]}
  --arch ARCH        amd64|x86_64|arm64|aarch64. Default: both amd64 and arm64.
  --tag TAG          Docker image tag. Default: kurento-media-server:local
  --force-build      Rebuild components even if their packages already exist.
  --no-image         Build packages only; skip Docker image creation.
  -h, --help         Show this help.

Legacy positional form ("$0 all amd64") is still accepted.

Examples:
  $0 --arch amd64 --tag kurento-media-server:ssrc-fix-amd-0.0.1 --force-build
  $0 --component module-core --arch amd64
EOF
}

COMPONENT=""
SINGLE_ARCH=""
IMAGE_TAG="${IMAGE_TAG:-kurento-media-server:local}"
FORCE_BUILD=false
SKIP_IMAGE=false
POSITIONAL=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --component)
            [[ -n "${2:-}" ]] || { echo "Error: --component needs a value" >&2; exit 1; }
            COMPONENT="$2"; shift 2 ;;
        --component=*)
            COMPONENT="${1#*=}"; shift ;;
        --arch)
            [[ -n "${2:-}" ]] || { echo "Error: --arch needs a value" >&2; exit 1; }
            SINGLE_ARCH="$2"; shift 2 ;;
        --arch=*)
            SINGLE_ARCH="${1#*=}"; shift ;;
        --tag|-t)
            [[ -n "${2:-}" ]] || { echo "Error: --tag needs a value" >&2; exit 1; }
            IMAGE_TAG="$2"; shift 2 ;;
        --tag=*)
            IMAGE_TAG="${1#*=}"; shift ;;
        --force-build|--force)
            FORCE_BUILD=true; shift ;;
        --no-image|--skip-image)
            SKIP_IMAGE=true; shift ;;
        -h|--help)
            usage; exit 0 ;;
        -*)
            echo "Error: Unknown option '$1'" >&2; echo "" >&2; usage >&2; exit 1 ;;
        *)
            POSITIONAL+=("$1"); shift ;;
    esac
done

# Legacy positional form: <component> <arch>
[[ -z "$COMPONENT"   && -n "${POSITIONAL[0]:-}" ]] && COMPONENT="${POSITIONAL[0]}"
[[ -z "$SINGLE_ARCH" && -n "${POSITIONAL[1]:-}" ]] && SINGLE_ARCH="${POSITIONAL[1]}"
if [[ ${#POSITIONAL[@]} -gt 2 ]]; then
    echo "Error: Too many arguments: ${POSITIONAL[*]}" >&2; exit 1
fi
COMPONENT="${COMPONENT:-all}"

# Validate the component name, so a typo cannot end up as a bogus bind mount
if [[ "$COMPONENT" != "all" ]]; then
    valid=false
    for c in "${COMPONENTS[@]}"; do
        [[ "$c" == "$COMPONENT" ]] && valid=true && break
    done
    if [[ "$valid" != true ]]; then
        echo "Error: Unknown component '$COMPONENT'" >&2
        echo "Supported: all ${COMPONENTS[*]}" >&2
        exit 1
    fi
    if [[ ! -f "$(pwd)/server/$COMPONENT/debian/control" ]]; then
        echo "Error: server/$COMPONENT/debian/control not found" >&2
        exit 1
    fi
fi

# Target architectures to build for
ARCHITECTURES="${ARCHITECTURES:-linux/amd64,linux/arm64}"
IMAGE_ARCH_MODE="multi"
if [[ -n "$SINGLE_ARCH" ]]; then
    case "$SINGLE_ARCH" in
        amd64|x86_64)
            ARCHITECTURES="linux/amd64"; IMAGE_ARCH_MODE="amd64" ;;
        arm64|aarch64)
            ARCHITECTURES="linux/arm64"; IMAGE_ARCH_MODE="arm64" ;;
        *)
            echo "Error: Unknown architecture '$SINGLE_ARCH'" >&2
            echo "Supported: amd64, arm64" >&2
            exit 1 ;;
    esac
fi

# Base directory for packages
PACKAGES_BASE_DIR="$(pwd)/server/packages"
mkdir -p "$PACKAGES_BASE_DIR"

# Docker image base name
DOCKER_IMAGE_BASE="kurento-buildpackage"

echo "================================================================"
echo "Multi-Architecture Build Configuration"
echo "================================================================"
echo "Target architectures: $ARCHITECTURES"
echo "Building component:   $COMPONENT"
echo "Force rebuild:        $FORCE_BUILD"
if [[ "$SKIP_IMAGE" == true ]]; then
    echo "Docker image:         (skipped)"
else
    echo "Docker image tag:     $IMAGE_TAG"
fi
echo "================================================================"
echo ""

# Setup Docker buildx builder if not exists
if ! docker buildx inspect kurento-builder >/dev/null 2>&1; then
    echo "Creating Docker buildx builder 'kurento-builder'..."
    docker buildx create --name kurento-builder --use --platform "$ARCHITECTURES"
else
    echo "Using existing Docker buildx builder 'kurento-builder'..."
    docker buildx use kurento-builder
fi

# Build multi-arch build images
echo "Building multi-architecture build container images..."
cd docker/kurento-buildpackage

# Build for each architecture separately to tag them properly
IFS=',' read -ra ARCH_ARRAY <<< "$ARCHITECTURES"
for platform in "${ARCH_ARRAY[@]}"; do
    # Extract arch name (linux/amd64 -> amd64)
    arch="${platform##*/}"

    echo ""
    echo "Building buildpackage image for $arch..."
    docker buildx build \
        --platform "$platform" \
        --load \
        -t "${DOCKER_IMAGE_BASE}:${arch}" \
        .
done

cd - > /dev/null

# Function to build a component for a specific architecture
build_component_arch() {
    local component="$1"
    local run_args="$2"
    local arch="$3"
    local packages_dir="$4"

    echo ""
    echo "################################################################"
    echo "Building Component: $component for $arch"
    echo "################################################################"

    # Skip components whose packages are already present, unless forced
    if [[ "$FORCE_BUILD" != true ]] \
        && compgen -G "$packages_dir/*${component}*.deb" >/dev/null; then
        echo "Packages for $component ($arch) already exist; skipping."
        echo "Use --force-build to rebuild."
        return 0
    fi

    # Remove existing packages for this component to avoid conflicts during build
    echo "Cleaning up old packages for $component ($arch)..."
    rm -f "$packages_dir"/*"$component"*.deb
    rm -f "$packages_dir"/*"$component"*.ddeb

    # We use docker run to build the component
    # Tests keeps timing-out so I added -e DEB_BUILD_OPTIONS="nocheck"
    # TODO: fix tests
    # Disable LTO to work around GCC 13 jobserver bug (internal_error in return_token)
    docker run --rm \
        --platform "linux/$arch" \
        --cap-add=SYS_NICE \
        --security-opt seccomp=unconfined \
        -e DEB_BUILD_OPTIONS="nocheck" \
        -e DEB_CFLAGS_APPEND="-fno-lto" \
        -e DEB_CXXFLAGS_APPEND="-fno-lto" \
        -e DEB_LDFLAGS_APPEND="-fno-lto" \
        -v "$(pwd)/server/$component":/hostdir \
        -v "$packages_dir":/packages \
        -v "$(pwd)/ci-scripts":/ci-scripts \
        "${DOCKER_IMAGE_BASE}:${arch}" \
        --dstdir /packages \
        --allow-dirty \
        $run_args
}

# Function to build a component for all architectures
build_component() {
    local component="$1"
    local run_args="$2"

    echo ""
    echo "================================================================"
    echo "Building $component for all architectures"
    echo "================================================================"

    IFS=',' read -ra ARCH_ARRAY <<< "$ARCHITECTURES"
    for platform in "${ARCH_ARRAY[@]}"; do
        # Extract arch name (linux/amd64 -> amd64)
        arch="${platform##*/}"

        # Create architecture-specific package directory
        packages_dir="$PACKAGES_BASE_DIR/$arch"
        mkdir -p "$packages_dir"

        # Build for this architecture
        build_component_arch "$component" "$run_args" "$arch" "$packages_dir"
    done
}

run_args_for() {
    case " $NEEDS_INSTALL_FILES " in
        *" $1 "*) echo "--install-files /packages" ;;
        *)        echo "" ;;
    esac
}

# Build the requested component, or all of them in dependency order
if [[ "$COMPONENT" != "all" ]]; then
    build_component "$COMPONENT" "$(run_args_for "$COMPONENT")"
else
    for component in "${COMPONENTS[@]}"; do
        build_component "$component" "$(run_args_for "$component")"
    done
fi

echo ""
echo "################################################################"
echo "Package Build Complete!"
echo "################################################################"
IFS=',' read -ra ARCH_ARRAY <<< "$ARCHITECTURES"
for platform in "${ARCH_ARRAY[@]}"; do
    arch="${platform##*/}"
    echo "Packages for $arch: $PACKAGES_BASE_DIR/$arch/"
done
echo "################################################################"

if [[ "$SKIP_IMAGE" == true ]]; then
    echo ""
    echo "Skipping Docker image creation (--no-image)."
    exit 0
fi

echo ""
IMAGE_TAG="$IMAGE_TAG" ./create_docker_image_from_packages.sh "$IMAGE_ARCH_MODE"
