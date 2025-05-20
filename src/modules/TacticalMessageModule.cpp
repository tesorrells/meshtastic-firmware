#include "configuration.h"
#if HAS_SCREEN

#include "TacticalMessageModule.h"
#include "Channels.h"
#include "FSCommon.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "PowerFSM.h"
#include "SPILock.h"
#include "detect/ScanI2C.h"
#include "input/ScanAndSelect.h"
#include "mesh/generated/meshtastic/cannedmessages.pb.h" // May not be needed if not using canned message specific protos
#include "modules/AdminModule.h"
#include "main.h"
#include "modules/ExternalNotificationModule.h"
#if !MESHTASTIC_EXCLUDE_GPS
#include "GPS.h"
#endif
#if defined(USE_EINK) && defined(USE_EINK_DYNAMICDISPLAY)
#include "graphics/EInkDynamicDisplay.h"
#endif

#include "graphics/ScreenFonts.h"
#include <Throttle.h>

// How long to wait before automatically deactivating the module UI if no interaction
#define TACTICAL_INACTIVATE_AFTER_MS 30000
#define TACTICAL_FEEDBACK_DURATION_MS 2000

// Global pointer to the module instance
TacticalMessageModule *tacticalMessageModule;

// Define the lists
const char *TacticalMessageModule::contacts[TACTICAL_CONTACTS_COUNT] = {"Inf", "Vec", "Obj", "FS", "LP/OP", "Comm", "FOB", "Friend", "Unkn"};
const char *TacticalMessageModule::distances[TACTICAL_DISTANCES_COUNT] = {"<25m", "25m", "50m", "100m", "150m", "200m", "300m", "400m", "500m"};
const char *TacticalMessageModule::orders[TACTICAL_ORDERS_COUNT] = {"ENGAGE", "Observe", "Retreat", "Follow", "Mark", "Regroup", "Dig-in", "Spread out", "Hold"};

TacticalMessageModule::TacticalMessageModule()
    : SinglePortModule("tactical", meshtastic_PortNum_TEXT_MESSAGE_APP), // Using TEXT_MESSAGE_APP port, adjust if needed
      concurrency::OSThread("TacticalMsg")
{
    commonInit();
    if (isEnabledViaConfig()) {
        LOG_INFO("TacticalMessageModule is enabled");
        this->inputObserver.observe(inputBroker);
        // If there's a way to configure this module via proto, load it here.
        // For now, we assume it's always enabled if HAS_SCREEN.
    } else {
        LOG_INFO("TacticalMessageModule is disabled (e.g., no screen or specific config)");
        currentStage = TACTICAL_MESSAGE_STAGE_DISABLED;
        disable(); // Method from Module base class
    }
}

TacticalMessageModule::~TacticalMessageModule()
{
    // Cleanup, if necessary
}

void TacticalMessageModule::commonInit() {
    // Initialization common to constructor and potentially other places
    tacticalMessageModule = this; // Assign the global pointer
    resetSelections();
}

bool TacticalMessageModule::isEnabledViaConfig() {
#if HAS_SCREEN
    // Check if the specific config for this module exists and is enabled.
    // moduleConfig is typically a global variable holding the current configurations.
    // Make sure "configuration.h" (or equivalent that defines moduleConfig) is included.
    if (moduleConfig.has_tactical_message) { // Check if the tactical_message field is set in the active config
        // If tactical_message config section exists, its 'enabled' field determines the state.
        // For an 'optional bool', if not explicitly set by the user, it defaults to false.
        return moduleConfig.tactical_message.enabled;
    }
    // If the tactical_message config section itself is not present, the module is considered disabled.
    return false;
#else
    return false; // No screen, so module is inherently disabled.
#endif
}

void TacticalMessageModule::resetSelections()
{
    selectedContactIndex = -1;
    selectedDistanceIndex = -1;
    selectedOrderIndex = -1;
    currentListItemIndex = 0;
    currentStage = TACTICAL_MESSAGE_STAGE_INACTIVE;
    memset(selectedContact, 0, sizeof(selectedContact));
    memset(selectedDistance, 0, sizeof(selectedDistance));
    memset(selectedOrder, 0, sizeof(selectedOrder));
    memset(constructedMessage, 0, sizeof(constructedMessage));
    lastInteractionTime = millis();
}

int TacticalMessageModule::handleInputEvent(const InputEvent *event)
{
    if (currentStage == TACTICAL_MESSAGE_STAGE_DISABLED || currentStage == TACTICAL_MESSAGE_STAGE_SENDING)
    {
        return 0; // Ignore input when disabled or sending
    }

    // Activation from INACTIVE stage (when module screen is shown)
    if (currentStage == TACTICAL_MESSAGE_STAGE_INACTIVE && 
        event->inputEvent == static_cast<char>(meshtastic_ModuleConfig_CannedMessageConfig_InputEventChar_SELECT) && 
        strcmp(event->source, "button") == 0
    ) {
        LOG_DEBUG("TacticalMessageModule activated by user from INACTIVE screen.");
        currentStage = TACTICAL_MESSAGE_STAGE_CONTACT; // Move to the first selection stage
        currentListItemIndex = 0;
        selectedContactIndex = -1; // Ensure selections are reset
        selectedDistanceIndex = -1;
        selectedOrderIndex = -1;
        lastInteractionTime = millis();
        requestFocus(); // Explicitly request focus for our module's UI interaction
        UIFrameEvent e;
        e.action = UIFrameEvent::Action::REGENERATE_FRAMESET; // Refresh the screen for the new stage
        this->notifyObservers(&e);
        runOnce(); // Process immediately to update display based on new stage
        return 0; // Event handled
    }

    // If still inactive after the above check, or not an activation event, ignore further processing for INACTIVE.
    if (currentStage == TACTICAL_MESSAGE_STAGE_INACTIVE) return 0; 

    bool processedEvent = false;
    lastInteractionTime = millis();

    // Handle UP/DOWN for list navigation
    if (event->inputEvent == static_cast<char>(meshtastic_ModuleConfig_CannedMessageConfig_InputEventChar_UP)) {
        retreatItem(getCurrentListSize());
        processedEvent = true;
    }
    else if (event->inputEvent == static_cast<char>(meshtastic_ModuleConfig_CannedMessageConfig_InputEventChar_DOWN)) {
        advanceItem(getCurrentListSize());
        processedEvent = true;
    }
    // Handle SELECT for choosing an item and moving to the next stage
    else if (event->inputEvent == static_cast<char>(meshtastic_ModuleConfig_CannedMessageConfig_InputEventChar_SELECT)) {
        if (currentStage == TACTICAL_MESSAGE_STAGE_CONTACT) {
            selectedContactIndex = currentListItemIndex;
            strncpy(selectedContact, contacts[selectedContactIndex], MAX_PART_LEN -1);
            nextStage();
        } else if (currentStage == TACTICAL_MESSAGE_STAGE_DISTANCE) {
            selectedDistanceIndex = currentListItemIndex;
            strncpy(selectedDistance, distances[selectedDistanceIndex], MAX_PART_LEN-1);
            nextStage();
        } else if (currentStage == TACTICAL_MESSAGE_STAGE_ORDER) {
            selectedOrderIndex = currentListItemIndex;
            strncpy(selectedOrder, orders[selectedOrderIndex], MAX_PART_LEN-1);
            // All selections made, prepare to send
            snprintf(constructedMessage, MAX_TACTICAL_MESSAGE_LEN, "%s %s %s", selectedContact, selectedDistance, selectedOrder);
            LOG_INFO("Constructed Tactical Message: %s", constructedMessage);
            currentStage = TACTICAL_MESSAGE_STAGE_SENDING;
            sendConstructedMessage(); 
        }
        processedEvent = true;
    }
    // Handle CANCEL/BACK to go to previous stage or deactivate
    else if (event->inputEvent == static_cast<char>(meshtastic_ModuleConfig_CannedMessageConfig_InputEventChar_CANCEL) ||
             event->inputEvent == static_cast<char>(meshtastic_ModuleConfig_CannedMessageConfig_InputEventChar_BACK)   
    ) {
        if (currentStage == TACTICAL_MESSAGE_STAGE_CONTACT) {
            // Already at the first stage, deactivate module
            currentStage = TACTICAL_MESSAGE_STAGE_INACTIVE;
            resetSelections();
        } else {
            previousStage();
        }
        processedEvent = true;
    }

    if (processedEvent) {
        requestFocus();
        UIFrameEvent e;
        e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
        this->notifyObservers(&e);
        runOnce(); // Update display immediately
    }
    return 0;
}

void TacticalMessageModule::nextStage() {
    currentListItemIndex = 0; // Reset list index for the new stage
    if (currentStage == TACTICAL_MESSAGE_STAGE_CONTACT) {
        currentStage = TACTICAL_MESSAGE_STAGE_DISTANCE;
    } else if (currentStage == TACTICAL_MESSAGE_STAGE_DISTANCE) {
        currentStage = TACTICAL_MESSAGE_STAGE_ORDER;
    } 
    // No next stage from ORDER other than SENDING, handled in input event
    lastInteractionTime = millis();
}

void TacticalMessageModule::previousStage() {
    currentListItemIndex = 0; // Reset list index for the new stage
    if (currentStage == TACTICAL_MESSAGE_STAGE_ORDER) {
        selectedOrderIndex = -1; // Clear selection for this stage
        memset(selectedOrder, 0, sizeof(selectedOrder));
        currentStage = TACTICAL_MESSAGE_STAGE_DISTANCE;
    } else if (currentStage == TACTICAL_MESSAGE_STAGE_DISTANCE) {
        selectedDistanceIndex = -1; // Clear selection for this stage
        memset(selectedDistance, 0, sizeof(selectedDistance));
        currentStage = TACTICAL_MESSAGE_STAGE_CONTACT;
    }
    // No previous stage from CONTACT other than INACTIVE, handled in input event
    lastInteractionTime = millis();
}

void TacticalMessageModule::advanceItem(int listMax) {
    if (listMax == 0) return;
    currentListItemIndex = (currentListItemIndex + 1) % listMax;
    lastInteractionTime = millis();
}

void TacticalMessageModule::retreatItem(int listMax) {
    if (listMax == 0) return;
    currentListItemIndex = (currentListItemIndex - 1 + listMax) % listMax;
    lastInteractionTime = millis();
}

const char** TacticalMessageModule::getCurrentList() {
    switch (currentStage) {
        case TACTICAL_MESSAGE_STAGE_CONTACT: return contacts;
        case TACTICAL_MESSAGE_STAGE_DISTANCE: return distances;
        case TACTICAL_MESSAGE_STAGE_ORDER: return orders;
        default: return nullptr;
    }
}

int TacticalMessageModule::getCurrentListSize() {
    switch (currentStage) {
        case TACTICAL_MESSAGE_STAGE_CONTACT: return TACTICAL_CONTACTS_COUNT;
        case TACTICAL_MESSAGE_STAGE_DISTANCE: return TACTICAL_DISTANCES_COUNT;
        case TACTICAL_MESSAGE_STAGE_ORDER: return TACTICAL_ORDERS_COUNT;
        default: return 0;
    }
}

// Optional: Helper to get a pointer to the current selection index variable if needed elsewhere
int* TacticalMessageModule::getCurrentSelectionIndexVar() {
    switch (currentStage) {
        case TACTICAL_MESSAGE_STAGE_CONTACT: return &selectedContactIndex;
        case TACTICAL_MESSAGE_STAGE_DISTANCE: return &selectedDistanceIndex;
        case TACTICAL_MESSAGE_STAGE_ORDER: return &selectedOrderIndex;
        default: return nullptr;
    }
}


void TacticalMessageModule::sendConstructedMessage()
{
    if (strlen(constructedMessage) == 0) {
        LOG_ERROR("Tactical message is empty, not sending.");
        showTemporaryFeedback("Error: Empty Msg", true);
        currentStage = TACTICAL_MESSAGE_STAGE_ORDER; // Go back to order selection
        return;
    }

    LOG_INFO("Sending tactical message: %s", constructedMessage);
    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p) {
        LOG_ERROR("Failed to allocate packet for tactical message");
        showTemporaryFeedback("Error: No Packet", true);
        currentStage = TACTICAL_MESSAGE_STAGE_ORDER;
        return;
    }

    p->to = NODENUM_BROADCAST; // Or a selected destination if you implement destination selection
    p->channel = channels.getPrimaryIndex(); // Or a selected channel
    p->want_ack = false; // Set to true if you want ACKs
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP; // Ensure this is the correct portnum
    p->decoded.payload.size = strlen(constructedMessage);
    memcpy(p->decoded.payload.bytes, constructedMessage, p->decoded.payload.size);
    
    // Optional: Add BELL character like CannedMessageModule
    // if (moduleConfig.canned_message.send_bell && p->decoded.payload.size < meshtastic_Constants_DATA_PAYLOAD_LEN) {
    //     p->decoded.payload.bytes[p->decoded.payload.size] = 7; // Bell character
    //     p->decoded.payload.bytes[p->decoded.payload.size + 1] = '\0';
    //     p->decoded.payload.size++;
    // }

    service->sendToMesh(p, RX_SRC_LOCAL, true); // send to mesh, cc to phone.
    showTemporaryFeedback("Sent!");
    
    // After sending, reset to inactive or first stage
    resetSelections();
    currentStage = TACTICAL_MESSAGE_STAGE_INACTIVE; // Or TACTICAL_MESSAGE_STAGE_CONTACT to allow another quick message

    UIFrameEvent e;
    e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
    this->notifyObservers(&e);
}

int32_t TacticalMessageModule::runOnce()
{
    if (currentStage == TACTICAL_MESSAGE_STAGE_DISABLED) {
        return INT32_MAX; // Sleep indefinitely
    }

    if (currentStage == TACTICAL_MESSAGE_STAGE_FEEDBACK) {
        if (millis() > feedbackEndTime) {
            temporaryFeedbackMessage = "";
            feedbackIsError = false;
            // Potentially go back to a specific stage or inactive
            if (currentStage != TACTICAL_MESSAGE_STAGE_SENDING) { // Avoid race condition if sending failed and set feedback
                 currentStage = TACTICAL_MESSAGE_STAGE_INACTIVE; 
            }
            UIFrameEvent e; 
            e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
            this->notifyObservers(&e);
        }
        return 100; // Check frequently to clear feedback message
    }

    // Auto-deactivate if idle
    if ((currentStage == TACTICAL_MESSAGE_STAGE_CONTACT || 
         currentStage == TACTICAL_MESSAGE_STAGE_DISTANCE || 
         currentStage == TACTICAL_MESSAGE_STAGE_ORDER) && 
        !Throttle::isWithinTimespanMs(lastInteractionTime, TACTICAL_INACTIVATE_AFTER_MS)
    ) {
        LOG_DEBUG("TacticalMessageModule auto-deactivating due to inactivity.");
        resetSelections();
        currentStage = TACTICAL_MESSAGE_STAGE_INACTIVE;
        UIFrameEvent e;
        e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
        this->notifyObservers(&e);
        return INT32_MAX;
    }
    
    // If sending was just triggered by input, runOnce might be called before send is complete.
    // Actual sending is quick, but feedback state is handled by showTemporaryFeedback.
    if (currentStage == TACTICAL_MESSAGE_STAGE_SENDING) {
        // This state is transient, sendConstructedMessage handles moving to FEEDBACK or INACTIVE
        // If sendConstructedMessage has an issue and doesn't change state, this is a fallback.
        if(temporaryFeedbackMessage.length() == 0) { // if no feedback set by send function, means it's still processing or issue.
            currentStage = TACTICAL_MESSAGE_STAGE_INACTIVE; // Fallback to inactive.
             UIFrameEvent e; 
            e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
            this->notifyObservers(&e);
        }
        return 200; // Give some time for send and feedback
    }

    return TACTICAL_INACTIVATE_AFTER_MS / 2; // Default check interval
}


bool TacticalMessageModule::shouldDraw()
{
    if (currentStage == TACTICAL_MESSAGE_STAGE_DISABLED) return false;
    if (temporaryFeedbackMessage.length() > 0) return true; // Always draw feedback
    // If the module is generally enabled (not disabled) and not showing feedback,
    // it should be considered drawable. The INACTIVE stage will show a prompt.
    return true; 
}


void TacticalMessageModule::showTemporaryFeedback(const String &message, bool isError)
{
    temporaryFeedbackMessage = message;
    feedbackIsError = isError;
    feedbackEndTime = millis() + TACTICAL_FEEDBACK_DURATION_MS;
    currentStage = TACTICAL_MESSAGE_STAGE_FEEDBACK;
    
    UIFrameEvent e;
    e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
    notifyObservers(&e);
    // Ensure runOnce is called soon to handle feedback timeout
    setIntervalFromNow(100);
}


bool TacticalMessageModule::interceptingKeyboardInput()
{
    // Intercept input if the module is in an active selection stage
    return currentStage == TACTICAL_MESSAGE_STAGE_CONTACT ||
           currentStage == TACTICAL_MESSAGE_STAGE_DISTANCE ||
           currentStage == TACTICAL_MESSAGE_STAGE_ORDER;
}

#if !HAS_TFT
void TacticalMessageModule::drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    char buffer[64]; // Increased buffer size for longer strings

    if (temporaryFeedbackMessage.length() > 0) {
        display->setTextAlignment(TEXT_ALIGN_CENTER);
        display->setFont(FONT_MEDIUM);
        display->drawString(display->getWidth() / 2 + x, display->getHeight() / 2 - FONT_HEIGHT_MEDIUM / 2 + y, temporaryFeedbackMessage);
        return;
    }

    if (currentStage == TACTICAL_MESSAGE_STAGE_DISABLED) {
        display->setTextAlignment(TEXT_ALIGN_CENTER);
        display->setFont(FONT_SMALL);
        display->drawString(display->getWidth() / 2 + x, display->getHeight() / 2 - FONT_HEIGHT_SMALL / 2 + y, "Tactical Msg Disabled");
        return;
    }
    
    if (currentStage == TACTICAL_MESSAGE_STAGE_INACTIVE) {
        display->setTextAlignment(TEXT_ALIGN_CENTER);
        display->setFont(FONT_MEDIUM); // Or FONT_LARGE if available and fits
        display->drawString(display->getWidth() / 2 + x, display->getHeight() / 2 - FONT_HEIGHT_MEDIUM / 2 + y, "Tactical Msgs");
        // Optionally, add a small "Press SEL" prompt if it fits well with the MEDIUM font title
        // display->setFont(FONT_SMALL);
        // display->drawString(display->getWidth() / 2 + x, display->getHeight() / 2 + FONT_HEIGHT_MEDIUM / 2 + 2 + y, "(SELECT to start)");
        return;
    }

    const char **currentList = getCurrentList();
    int listSize = getCurrentListSize();
    if (!currentList || listSize == 0) return; // Should not happen in active stages

    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL); // Using FONT_SMALL for lists

    // Display current stage or selection path as a header
    String header = "";
    if (selectedContactIndex != -1) header += contacts[selectedContactIndex];
    if (selectedDistanceIndex != -1) { 
        if(header.length() > 0) header += " > ";
        header += distances[selectedDistanceIndex];
    }
    // Don't add order to header, as it's the final selection before send
    if (header.length() > 0 && (currentStage == TACTICAL_MESSAGE_STAGE_DISTANCE || currentStage == TACTICAL_MESSAGE_STAGE_ORDER)) {
        display->drawStringMaxWidth(x, y, display->getWidth(), header);
    } else {
         // Show current stage title if no selections yet for that part of header
        switch(currentStage) {
            case TACTICAL_MESSAGE_STAGE_CONTACT: display->drawString(x,y, "Select Contact:"); break;
            case TACTICAL_MESSAGE_STAGE_DISTANCE: display->drawString(x,y, "Select Distance:"); break;
            case TACTICAL_MESSAGE_STAGE_ORDER: display->drawString(x,y, "Select Order:"); break;
            default: break;
        }
    }

    int headerLines = 1;
    int itemLineHeight = FONT_HEIGHT_SMALL + 2; // Add a little padding
    int maxItemsOnScreen = (display->getHeight() - (y + (FONT_HEIGHT_SMALL * headerLines))) / itemLineHeight;
    maxItemsOnScreen = std::max(1, maxItemsOnScreen); // Ensure at least one item can be shown

    int startIdx = 0;
    if (currentListItemIndex >= maxItemsOnScreen) {
        startIdx = currentListItemIndex - maxItemsOnScreen + 1;
    }

    for (int i = 0; i < maxItemsOnScreen && (startIdx + i) < listSize; ++i) {
        int actualIndex = startIdx + i;
        int lineY = y + (FONT_HEIGHT_SMALL * headerLines) + (i * itemLineHeight);

        if (actualIndex == currentListItemIndex) {
            // Highlight selected item
            // For non-EINK, invert colors. For EINK, prefix with '>'.
#ifdef USE_EINK
            snprintf(buffer, sizeof(buffer), "> %s", currentList[actualIndex]);
            display->drawString(x + 2, lineY, buffer);
#else
            display->fillRect(x, lineY -1, display->getWidth(), itemLineHeight -1 );
            display->setColor(BLACK);
            display->drawString(x + 5, lineY, currentList[actualIndex]);
            display->setColor(WHITE);
#endif
        } else {
            display->drawString(x + 5, lineY, currentList[actualIndex]);
        }
    }
}
#endif // !HAS_TFT

#endif // HAS_SCREEN
