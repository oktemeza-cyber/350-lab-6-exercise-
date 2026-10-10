#include "mandelbrot.h"

#include <cassert>
#include <cmath>
#include <memory>
#include <sstream>

#include "cyclic_gradient.h"
#include "open_sans_semibold.h"
#include "vector_util.h"

// For use by our constructor.
// Returns the appropriate initial world bounds.
// Should always contain the coordinates bounded in (-2.5, -1.5) to (0.5, 1.5).
std::pair<sf::Vector2<double>, sf::Vector2<double>>
MandelbrotViewer::getInitialWorldBoundsForWindowSize(unsigned int windowWidth,
                                                     unsigned int windowHeight) {
    assert(windowWidth > 0);
    assert(windowHeight > 0);
    const double aspectRatio = static_cast<double>(windowWidth) / windowHeight;
    const sf::Vector2<double> worldCenter{-1., 0.};
    sf::Vector2<double> initWorldSize = {3., 3.};
    if (windowWidth >= windowHeight) {
        // world height will be 3
        initWorldSize.x = 3 * aspectRatio;
    } else {
        // world width will be 3
        initWorldSize.y = 3 / aspectRatio;
    }
    return {worldCenter - initWorldSize / 2., worldCenter + initWorldSize / 2.};
}

MandelbrotViewer::MandelbrotViewer(unsigned int windowWidth, unsigned int windowHeight)
    : mWindow(sf::VideoMode(sf::Vector2u{windowWidth, windowHeight}), "Mandelbrot set viewer"),
      mWindowSize(windowWidth, windowHeight),
      mFont(EmbeddedFonts::OpenSans_SemiBold_ttf, EmbeddedFonts::OpenSans_SemiBold_ttf_len),
      mCursorWorldPosText(mFont),
      mCursorWorldPosTextShadow(mFont),
      mViewBuffer(sf::Vector2u{windowWidth, windowHeight}, sf::Color::Blue),
      mViewBufferGPU(mViewBuffer),
      mViewSprite(mViewBufferGPU) {
    assert(windowWidth > 0);
    assert(windowHeight > 0);
    std::tie(mMinPointWorld, mMaxPointWorld) =
        getInitialWorldBoundsForWindowSize(windowWidth, windowHeight);

    // disable key repeat
    mWindow.setKeyRepeatEnabled(false);
    mWindow.setFramerateLimit(60);

    // Initially no text to show
    mCursorWorldPosText.setString("");
    mCursorWorldPosTextShadow.setString("");
    // Character size of 14 pixels
    mCursorWorldPosText.setCharacterSize(14);
    mCursorWorldPosTextShadow.setCharacterSize(14);
    // Text is white, shadow is black.
    mCursorWorldPosText.setFillColor(sf::Color::White);
    mCursorWorldPosTextShadow.setFillColor(sf::Color(0, 0, 0, 130));
    // Set text position
    mCursorWorldPosText.setPosition({10, 10});
    mCursorWorldPosTextShadow.setPosition({10, 10});
    // Give text shadow extra thickness.
    mCursorWorldPosTextShadow.setOutlineColor(sf::Color(0, 0, 0, 130));
    mCursorWorldPosTextShadow.setOutlineThickness(3);
}

void MandelbrotViewer::run() {
    sf::Clock clock;
    sf::Time lastTime = sf::Time();
    int maxIters = MAX_ITERS_LOWER_BOUND;
    int renderedMaxIters = -1;
    while (true) {
        sf::Time curTime = clock.getElapsedTime();
        // Handle inputs
        InputSummary inputSummary = readInputs();
        if (inputSummary.shouldClose) {
            mWindow.close();
            return;
        }
        // Determine whether any of the inputs cause the view to change.
        bool viewChanged = (inputSummary.zoomDistance != 0) ||
                           (inputSummary.panVector != sf::Vector2<double>{0., 0.}) ||
                           inputSummary.shouldResize;
        // Update
        updateViewState(inputSummary, curTime - lastTime);
        updateUIText(inputSummary.mousePosition);
        maxIters = (viewChanged ? MAX_ITERS_LOWER_BOUND
                                : std::min(maxIters * ITERS_MULTIPLIER, MAX_ITERS_UPPER_BOUND));
        // Render:
        if (viewChanged || maxIters != renderedMaxIters) {
            drawIntoViewBuffer(maxIters);
            copyViewBufferToGPU();
            renderedMaxIters = maxIters;
        }
        draw();
        // update timing stats
        lastTime = curTime;
    }
}

MandelbrotViewer::InputSummary MandelbrotViewer::readInputs() {
    InputSummary ret{};
    while (const std::optional<sf::Event> event = mWindow.pollEvent()) {
        if (event->is<sf::Event::Closed>()) {
            ret.shouldClose = true;
            return ret;  // just stop early
        } else if (const auto* mouseScrolled = event->getIf<sf::Event::MouseWheelScrolled>()) {
            // If multiple of these, it adds up.
            ret.zoomDistance += mouseScrolled->delta;
        } else if (const auto* resized = event->getIf<sf::Event::Resized>()) {
            ret.desiredWindowSize = resized->size;
            ret.shouldResize = true;
        }
    }
    // pan is a continuous action, so we'll just check current state for each key. Note, NOT
    // else if.
    if (sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::W)) {
        ret.panVector += {0., 1};
    }
    if (sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::S)) {
        ret.panVector += {0., -1.};
    }
    if (sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::A)) {
        ret.panVector += {-1., 0.};
    }
    if (sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::D)) {
        ret.panVector += {1., 0.};
    }
    ret.mousePosition = sf::Mouse::getPosition(mWindow);
    return ret;
}

void MandelbrotViewer::updateViewState(const InputSummary& inputs, sf::Time deltaTime) {
    if (inputs.shouldResize) {
        handleWindowResize(inputs.desiredWindowSize);
    }
    if (inputs.zoomDistance != 0) {
        handleZoom(inputs.zoomDistance, inputs.mousePosition);
    }
    if (inputs.panVector != sf::Vector2<double>{0., 0.}) {
        handlePans(inputs.panVector, deltaTime);
    }
}

void MandelbrotViewer::handleZoom(double scrollDistance, sf::Vector2i mousePosition) {
    double worldViewFactor = std::pow(ZOOM_EXPONENT_BASE, scrollDistance);
    sf::Vector2<double> worldPoint = windowPosToWorld(sf::Vector2<double>(mousePosition));

    double fx = static_cast<double>(mousePosition.x) / mWindowSize.x;
    double newWidth = (mMaxPointWorld.x - mMinPointWorld.x) * std::pow(ZOOM_EXPONENT_BASE, scrollDistance);
    double newMinX = (worldPoint.x - (fx*newWidth));
    double newMaxPointWorldX = (newMinX + newWidth);
    mMinPointWorld.x = newMinX;
    mMaxPointWorld.x = newMaxPointWorldX;

    double fy = static_cast<double>(mousePosition.y) / mWindowSize.y;
    double newHeight = (mMaxPointWorld.y - mMinPointWorld.y) * std::pow(ZOOM_EXPONENT_BASE, scrollDistance);
    double newMaxY = (worldPoint.y + (fy*newHeight));
    double newMinPointWorldY = (newMaxY - newHeight);
    mMinPointWorld.y = newMinPointWorldY;
    mMaxPointWorld.y = newMaxY ;

}

void MandelbrotViewer::handleWindowResize(sf::Vector2u newSize)  // newSize is in window coords.
{
    // IMPORTANT: after resizing window, need to fix view, or else original world
    // rectangle gets scaled to new window size instead. This line ensures that when the window
    // expands, "more of the world" is correspondingly viewable.
    mWindow.setView(sf::View(sf::FloatRect({0, 0}, {static_cast<float>(newSize.x), static_cast<float>(newSize.y)})));
    double centerX = (mMinPointWorld.x + mMaxPointWorld.x) / 2;
    double centerY = (mMinPointWorld.y + mMaxPointWorld.y) / 2;

    double scaleX = static_cast<double>(newSize.x )/ mWindowSize.x;
    double newWidth = scaleX * (mMaxPointWorld.x - mMinPointWorld.x);
    double newMinX = centerX - (newWidth/2);
    double newMaxX = centerX + (newWidth/2);

    mMinPointWorld.x = newMinX;
    mMaxPointWorld.x = newMaxX;

    double scaleY = static_cast<double>(newSize.y )/ mWindowSize.y;
    double newHeight = scaleY * (mMaxPointWorld.y - mMinPointWorld.y);
    double newMinY = centerY - (newHeight/2);
    double newMaxY = centerY + (newHeight/2);

    mMinPointWorld.y = newMinY;
    mMaxPointWorld.y = newMaxY;

    // update CPU-side image buffer size to have enough memory for all the pixels:
    mViewBuffer.resize(newSize);
     mViewBufferGPU.resize(newSize);
    // The sprite will have an incorrect view into the texture after resize, so we update:
    mViewSprite.setTextureRect(sf::IntRect({0, 0}, sf::Vector2i(newSize)));
    mWindowSize = newSize;  // update mWindowSize.
}

void MandelbrotViewer::handlePans(sf::Vector2<double> basePanVector, sf::Time deltaTime) {
    const sf::Vector2<double> worldViewSize = mMaxPointWorld - mMinPointWorld;
    const double panScale = (worldViewSize.x + worldViewSize.y) * 0.5;
    const auto panDelta =
        basePanVector * static_cast<double>(deltaTime.asMilliseconds()) * panScale * PAN_FACTOR;
    mMinPointWorld += panDelta;
    mMaxPointWorld += panDelta;
}

std::string MandelbrotViewer::formatCoords(const sf::Vector2<double>& pos) {
    std::stringstream ss;
    ss << std::setprecision(15) << "(" << pos.x << ", " << pos.y << ")";
    return ss.str();
}

// updateUIText updates mCursorWorldPosText and mCursorWorldPosTextShadow
// to show the cursor's current **world coordinates** position
void MandelbrotViewer::updateUIText(sf::Vector2i mouseWindowCoords) {
    auto coordsString = formatCoords(windowPosToWorld(sf::Vector2<double>(mouseWindowCoords)));
    mCursorWorldPosText.setString(coordsString);
    mCursorWorldPosTextShadow.setString(coordsString);
}

double MandelbrotViewer::mandelbrot(double cX, double cY, int maxIters) const {
    double zX = 0;
    double zY = 0;

    for(int n = 1; n <= maxIters; n++){
        double zPrimeX = zX*zX - zY*zY + cX;
        double zPrimeY = 2 * (zX * zY) + cY;

        zX = zPrimeX;
        zY = zPrimeY;
        if(zPrimeX * zPrimeX + zPrimeY * zPrimeY > 4) {
            return n;
        }
    }
    return std::numeric_limits<double>::infinity();
}

double MandelbrotViewer::mandelbrotSmooth(double cX, double cY, int maxIters) const {
    double zX = 0;
    double zY = 0;

    for(int n = 1; n <= maxIters; n++){
        double zPrimeX = zX*zX - zY*zY + cX;
        double zPrimeY = 2 * (zX * zY) + cY;

        zX = zPrimeX;
        zY = zPrimeY;
        if(zPrimeX * zPrimeX + zPrimeY * zPrimeY > 16) {
            double zN = std::sqrt((zPrimeX * zPrimeX) + (zPrimeY*zPrimeY));
            return (n + 1 - std::log(std::log(zN)) / LOG_2);
        }
    }
    return std::numeric_limits<double>::infinity();  
}

// windowPosToWorld takes a point in window coordinates and converts it to world coordinates
sf::Vector2<double> MandelbrotViewer::windowPosToWorld(const sf::Vector2<double>& pWindow) {
    double fractionX = pWindow.x / mWindowSize.x;
    double worldX = mMinPointWorld.x + (fractionX * (mMaxPointWorld.x - mMinPointWorld.x));

    double fractionY = pWindow.y / mWindowSize.y;
    double worldY = mMaxPointWorld.y + (fractionY * (mMinPointWorld.y - mMaxPointWorld.y));

    return sf::Vector2<double>{worldX, worldY};

}

// drawIntoBuffer renders the current world view (bounded by mMinPointWorld and mMaxPointWorld)
// into mViewBuffer
void MandelbrotViewer::drawIntoViewBuffer(int maxIters) {
    for(unsigned int px = 0; px < mViewBuffer.getSize().x; px++){
        for(unsigned int py = 0; py < mViewBuffer.getSize().y; py++){
            sf::Vector2<double> world = windowPosToWorld({px + 0.5, py + 0.5});
            double escapeTime = mandelbrotSmooth(world.x, world.y, maxIters);
            sf::Color colour;
            if(escapeTime != std::numeric_limits<double>::infinity()){
                colour = CyclicGradient::DEFAULT_GRADIENT(escapeTime);
            } else {
                colour = sf::Color::Black;
            }
        mViewBuffer.setPixel(sf::Vector2u{px, py}, colour); 
    }
}
}

// copyViewBufferToGPU takes the drawn CPU-side buffer mViewBuffer and copies it to the
// GPU-side.
void MandelbrotViewer::copyViewBufferToGPU() {
    mViewBufferGPU.update(mViewBuffer);
}

// draw clears the window, draws the view, as well as the text with its shadow underneath
// it. Finally, the window is displayed.
void MandelbrotViewer::draw() {
    mWindow.clear();
    mWindow.draw(mViewSprite);
    mWindow.draw(mCursorWorldPosTextShadow);
    mWindow.draw(mCursorWorldPosText);
    mWindow.display();
}
