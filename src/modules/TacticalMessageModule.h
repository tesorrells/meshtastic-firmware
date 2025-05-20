#pragma once
#if HAS_SCREEN // Ensure a screen is available

#include "ProtobufModule.h"
#include "input/InputBroker.h"

// Define the stages for the tactical message builder
enum TacticalMessageStage {
    TACTICAL_MESSAGE_STAGE_DISABLED,
    TACTICAL_MESSAGE_STAGE_INACTIVE, // Module is active but not currently showing UI / waiting for trigger
    TACTICAL_MESSAGE_STAGE_CONTACT,
    TACTICAL_MESSAGE_STAGE_DISTANCE,
    TACTICAL_MESSAGE_STAGE_ORDER,
    TACTICAL_MESSAGE_STAGE_SENDING,
    TACTICAL_MESSAGE_STAGE_FEEDBACK // For showing "Sent" or "Error"
};

// How many items in each list
#define TACTICAL_CONTACTS_COUNT 9
#define TACTICAL_DISTANCES_COUNT 9
#define TACTICAL_ORDERS_COUNT 9

// Maximum length for a part of the message + null terminator
#define MAX_PART_LEN 10 
// Max length of the final constructed message: Contact + " " + Distance + " " + Order + Null = (MAX_PART_LEN-1)*3 + 2 + 1
#define MAX_TACTICAL_MESSAGE_LEN (MAX_PART_LEN * 3)


class TacticalMessageModule : public SinglePortModule, public Observable<const UIFrameEvent *>, private concurrency::OSThread
{
    CallbackObserver<TacticalMessageModule, const InputEvent *> inputObserver =
        CallbackObserver<TacticalMessageModule, const InputEvent *>(this, &TacticalMessageModule::handleInputEvent);

  public:
    TacticalMessageModule();
    ~TacticalMessageModule();

    bool shouldDraw();
    void showTemporaryFeedback(const String &message, bool isError = false);
    const char * getModuleName() { return "TacticalMessageModule"; }


#if defined(RAK14014) || defined(USE_VIRTUAL_KEYBOARD) // Example guards if specific hardware affects behavior
    TacticalMessageStage getStage() const { return currentStage; }
#endif

  protected:
    virtual int32_t runOnce() override;
    virtual bool wantUIFrame() override { return this->shouldDraw(); }
    virtual Observable<const UIFrameEvent *> *getUIFrameObservable() override { return this; }
    virtual bool interceptingKeyboardInput() override;

#if !HAS_TFT
    virtual void drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) override;
#endif

    int handleInputEvent(const InputEvent *event);
    void sendConstructedMessage();
    void resetSelections();
    void nextStage();
    void previousStage();
    void advanceItem(int listMax);
    void retreatItem(int listMax);
    const char** getCurrentList();
    int getCurrentListSize();
    int* getCurrentSelectionIndexVar();


    // Module state
    TacticalMessageStage currentStage = TACTICAL_MESSAGE_STAGE_INACTIVE;
    
    // Selections
    int selectedContactIndex = -1;
    int selectedDistanceIndex = -1;
    int selectedOrderIndex = -1;

    // Current item highlighted in the list
    int currentListItemIndex = 0; 

    // Storage for the selected strings (optional, could construct on the fly)
    char selectedContact[MAX_PART_LEN] = {0};
    char selectedDistance[MAX_PART_LEN] = {0};
    char selectedOrder[MAX_PART_LEN] = {0};
    char constructedMessage[MAX_TACTICAL_MESSAGE_LEN] = {0};

    String temporaryFeedbackMessage;
    bool feedbackIsError = false;
    unsigned long feedbackEndTime = 0;

    unsigned long lastInteractionTime = 0;

    // Predefined lists
    static const char *contacts[TACTICAL_CONTACTS_COUNT];
    static const char *distances[TACTICAL_DISTANCES_COUNT];
    static const char *orders[TACTICAL_ORDERS_COUNT];

    char payload = 0x00; // For specific key events if needed, similar to CannedMessageModule

  private:
    void commonInit();
    bool isEnabledViaConfig();
};

extern TacticalMessageModule *tacticalMessageModule;

#endif // HAS_SCREEN 