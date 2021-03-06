#include "pxt.h"
#include "neopixel.h"
#include <map>

CodalDevice device;

namespace pxt {

// The first two word are used to tell the bootloader that a single reset should start the
// bootloader and the MSD device, not us.
// The rest is reserved for partial flashing checksums.
__attribute__((section(".binmeta"))) __attribute__((used)) const uint32_t pxt_binmeta[] = {
    0x87eeb07c, 0x87eeb07c, 0x00ff00ff, 0x00ff00ff, 0x00ff00ff, 0x00ff00ff,
    0x00ff00ff, 0x00ff00ff, 0x00ff00ff, 0x00ff00ff, 0x00ff00ff, 0x00ff00ff,
    0x00ff00ff, 0x00ff00ff,
};

TValue incr(TValue e) {
    if (isRefCounted(e))
        ((RefObject *)e)->ref();
    return e;
}

void decr(TValue e) {
#if 0
        if (((RefCounted *)e)->refCount != 0xffff) {
            DMESG("DECR: %p refs=%d vt=%p", e, ((RefCounted *)e)->refCount,
                    ((RefCounted *)e)->vtablePtr);
        }
#endif

    if (isRefCounted(e))
        ((RefObject *)e)->unref();
}

Action mkAction(int reflen, int totallen, int startptr) {
    check(0 <= reflen && reflen <= totallen, ERR_SIZE, 1);
    check(reflen <= totallen && totallen <= 255, ERR_SIZE, 2);
    check(bytecode[startptr] == 0xffff, ERR_INVALID_BINARY_HEADER, 3);
    check(bytecode[startptr + 1] == REF_TAG_ACTION, ERR_INVALID_BINARY_HEADER, 4);

    uint32_t tmp = (uint32_t)&bytecode[startptr];

    if (totallen == 0) {
        return (TValue)tmp; // no closure needed
    }

    void *ptr = ::operator new(sizeof(RefAction) + totallen * sizeof(uint32_t));
    RefAction *r = new (ptr) RefAction();
    r->len = totallen;
    r->reflen = reflen;
    r->func = (ActionCB)((tmp + 4) | 1);
    memset(r->fields, 0, r->len * sizeof(uint32_t));

    return (Action)r;
}

TValue runAction3(Action a, TValue arg0, TValue arg1, TValue arg2) {
    auto aa = (RefAction *)a;
    if (aa->vtable == REF_TAG_ACTION) {
        check(aa->refcnt == 0xffff, ERR_INVALID_BINARY_HEADER, 4);
        return ((ActionCB)(((uint32_t)a + 4) | 1))(NULL, arg0, arg1, arg2);
    } else {
        check(aa->refcnt != 0xffff, ERR_INVALID_BINARY_HEADER, 4);
        return aa->runCore(arg0, arg1, arg2);
    }
}

TValue runAction2(Action a, TValue arg0, TValue arg1) {
    return runAction3(a, arg0, arg1, 0);
}

TValue runAction1(Action a, TValue arg0) {
    return runAction3(a, arg0, 0, 0);
}

TValue runAction0(Action a) {
    return runAction3(a, 0, 0, 0);
}

RefRecord *mkClassInstance(int vtableOffset) {
    VTable *vtable = (VTable *)&bytecode[vtableOffset];

    intcheck(vtable->methods[0] == &RefRecord_destroy, ERR_SIZE, 3);
    intcheck(vtable->methods[1] == &RefRecord_print, ERR_SIZE, 4);

    void *ptr = ::operator new(vtable->numbytes);
    RefRecord *r = new (ptr) RefRecord(PXT_VTABLE_TO_INT(vtable));
    memset(r->fields, 0, vtable->numbytes - sizeof(RefRecord));
    return r;
}

TValue RefRecord::ld(int idx) {
    // intcheck((reflen == 255 ? 0 : reflen) <= idx && idx < len, ERR_OUT_OF_BOUNDS, 1);
    return fields[idx];
}

TValue RefRecord::ldref(int idx) {
    // DMESG("LD %p len=%d reflen=%d idx=%d", this, len, reflen, idx);
    // intcheck(0 <= idx && idx < reflen, ERR_OUT_OF_BOUNDS, 2);
    TValue tmp = fields[idx];
    incr(tmp);
    return tmp;
}

void RefRecord::st(int idx, TValue v) {
    // intcheck((reflen == 255 ? 0 : reflen) <= idx && idx < len, ERR_OUT_OF_BOUNDS, 3);
    fields[idx] = v;
}

void RefRecord::stref(int idx, TValue v) {
    // DMESG("ST %p len=%d reflen=%d idx=%d", this, len, reflen, idx);
    // intcheck(0 <= idx && idx < reflen, ERR_OUT_OF_BOUNDS, 4);
    decr(fields[idx]);
    fields[idx] = v;
}

void RefObject::destroyVT() {
    ((RefObjectMethod)getVTable(this)->methods[0])(this);
    ::operator delete(this);
}

void RefObject::printVT() {
    ((RefObjectMethod)getVTable(this)->methods[1])(this);
}

void RefRecord_destroy(RefRecord *r) {
    VTable *tbl = getVTable(r);
    uint8_t *refmask = (uint8_t *)&tbl->methods[tbl->userdata & 0xff];
    int len = (tbl->numbytes >> 2) - 1;
    for (int i = 0; i < len; ++i) {
        if (refmask[i])
            decr(r->fields[i]);
        r->fields[i] = 0;
    }
}

void RefRecord_print(RefRecord *r) {
    DMESG("RefRecord %p r=%d size=%d bytes", r, r->refcnt, getVTable(r)->numbytes);
}

TValue Segment::get(uint32_t i) {
#ifdef DEBUG_BUILD
    DMESG("In Segment::get index:%d", i);
    this->print();
#endif

    if (i < length) {
        return data[i];
    }
    return Segment::DefaultValue;
}

void Segment::set(uint32_t i, TValue value) {
    if (i < size) {
        data[i] = value;
    } else if (i < Segment::MaxSize) {
        growByMin(i + 1);
        data[i] = value;
    }
    if (length <= i) {
        length = i + 1;
    }

#ifdef DEBUG_BUILD
    DMESG("In Segment::set");
    this->print();
#endif

    return;
}

uint16_t Segment::growthFactor(uint16_t size) {
    if (size == 0) {
        return 4;
    }
    if (size < 64) {
        return size * 2; // Double
    }
    if (size < 512) {
        return size * 5 / 3; // Grow by 1.66 rate
    }
    return size + 256; // Grow by constant rate
}

void Segment::growByMin(uint16_t minSize) {
    growBy(max(minSize, growthFactor(size)));
}

void Segment::growBy(uint16_t newSize) {
#ifdef DEBUG_BUILD
    DMESG("growBy: %d", newSize);
    this->print();
#endif
    if (size < newSize) {
        // this will throw if unable to allocate
        TValue *tmp = (TValue *)(::operator new(newSize * sizeof(TValue)));

        // Copy existing data
        if (size) {
            memcpy(tmp, data, size * sizeof(TValue));
        }
        // fill the rest with default value
        memset(tmp + size, 0, (newSize - size) * sizeof(TValue));

        // free older segment;
        ::operator delete(data);

        data = tmp;
        size = newSize;

#ifdef DEBUG_BUILD
        DMESG("growBy - after reallocation");
        this->print();
#endif
    }
    // else { no shrinking yet; }
    return;
}

void Segment::ensure(uint16_t newSize) {
    if (newSize < size) {
        return;
    }
    growByMin(newSize);
}

void Segment::setLength(uint32_t newLength) {
    if (newLength > size) {
        ensure(length);
    }
    length = newLength;
    return;
}

void Segment::push(TValue value) {
    this->set(length, value);
}

TValue Segment::pop() {
#ifdef DEBUG_BUILD
    DMESG("In Segment::pop");
    this->print();
#endif

    if (length > 0) {
        TValue value = data[length];
        data[length] = Segment::DefaultValue;
        --length;
        return value;
    }
    return Segment::DefaultValue;
}

// this function removes an element at index i and shifts the rest of the elements to
// left to fill the gap
TValue Segment::remove(uint32_t i) {
#ifdef DEBUG_BUILD
    DMESG("In Segment::remove index:%d", i);
    this->print();
#endif
    if (i < length) {
        // value to return
        TValue ret = data[i];
        if (i + 1 < length) {
            // Move the rest of the elements to fill in the gap.
            memmove(data + i, data + i + 1, (length - i - 1) * sizeof(uint32_t));
        }
        length--;
        data[length] = Segment::DefaultValue;
#ifdef DEBUG_BUILD
        DMESG("After Segment::remove index:%d", i);
        this->print();
#endif
        return ret;
    }
    return Segment::DefaultValue;
}

// this function inserts element value at index i by shifting the rest of the elements right.
void Segment::insert(uint32_t i, TValue value) {
#ifdef DEBUG_BUILD
    DMESG("In Segment::insert index:%d value:%d", i, value);
    this->print();
#endif

    if (i < length) {
        ensure(length + 1);
        if (i + 1 < length) {
            // Move the rest of the elements to fill in the gap.
            memmove(data + i + 1, data + i, (length - i) * sizeof(uint32_t));
        }

        data[i] = value;
        length++;
    } else {
        // This is insert beyond the length, just call set which will adjust the length
        set(i, value);
    }
#ifdef DEBUG_BUILD
    DMESG("After Segment::insert index:%d", i);
    this->print();
#endif
}

void Segment::print() {
    DMESG("Segment: %p, length: %d, size: %d", data, (uint32_t)length, (uint32_t)size);
    for (uint32_t i = 0; i < size; i++) {
        DMESG("-> %d", (uint32_t)data[i]);
    }
}

bool Segment::isValidIndex(uint32_t i) {
    if (i > length) {
        return false;
    }
    return true;
}

void Segment::destroy() {
#ifdef DEBUG_BUILD
    DMESG("In Segment::destroy");
    this->print();
#endif
    length = size = 0;
    ::operator delete(data);
    data = nullptr;
}

void RefCollection::push(TValue x) {
    incr(x);
    head.push(x);
}

TValue RefCollection::pop() {
    TValue ret = head.pop();
    incr(ret);
    return ret;
}

TValue RefCollection::getAt(int i) {
    TValue tmp = head.get(i);
    incr(tmp);
    return tmp;
}

TValue RefCollection::removeAt(int i) {
    decr(head.get(i));
    return head.remove(i);
}

void RefCollection::insertAt(int i, TValue value) {
    head.insert(i, value);
    incr(value);
}

void RefCollection::setAt(int i, TValue value) {
    if (head.isValidIndex((uint32_t)i))
        decr(head.get(i));
    incr(value);
    head.set(i, value);
}

int RefCollection::indexOf(TValue x, int start) {
    uint32_t i = start;
    while (head.isValidIndex(i)) {
        if (pxt::eq_bool(head.get(i), x)) {
            return (int)i;
        }
        i++;
    }
    return -1;
}

bool RefCollection::removeElement(TValue x) {
    int idx = indexOf(x, 0);
    if (idx >= 0) {
        removeAt(idx);
        return 1;
    }
    return 0;
}

namespace Coll0 {
PXT_VTABLE_BEGIN(RefCollection, 0, 0)
PXT_VTABLE_END
}

RefCollection::RefCollection() : RefObject(0) {
    vtable = PXT_VTABLE_TO_INT(&Coll0::RefCollection_vtable);
}

void RefCollection::destroy() {
    for (uint32_t i = 0; i < this->head.getLength(); i++) {
        decr(this->head.get(i));
    }
    this->head.destroy();
}

void RefCollection::print() {
    DMESG("RefCollection %p r=%d size=%d", this, refcnt, head.getLength());
    head.print();
}

PXT_VTABLE_CTOR(RefAction) {}

// fields[] contain captured locals
void RefAction::destroy() {
    for (int i = 0; i < this->reflen; ++i) {
        decr(fields[i]);
        fields[i] = 0;
    }
}

void RefAction::print() {
    DMESG("RefAction %p r=%d pc=%X size=%d (%d refs)", this, refcnt,
          (const uint8_t *)func - (const uint8_t *)bytecode, len, reflen);
}

void RefLocal::print() {
    DMESG("RefLocal %p r=%d v=%d", this, refcnt, v);
}

void RefLocal::destroy() {}

PXT_VTABLE_CTOR(RefLocal) {
    v = 0;
}

PXT_VTABLE_CTOR(RefRefLocal) {
    v = 0;
}

void RefRefLocal::print() {
    DMESG("RefRefLocal %p r=%d v=%p", this, refcnt, (void *)v);
}

void RefRefLocal::destroy() {
    decr(v);
}

PXT_VTABLE_BEGIN(RefMap, 0, RefMapMarker)
PXT_VTABLE_END
RefMap::RefMap() : PXT_VTABLE_INIT(RefMap) {}

void RefMap::destroy() {
    for (unsigned i = 0; i < data.size(); ++i) {
        if (data[i].key & 1) {
            decr(data[i].val);
        }
        data[i].val = 0;
    }
    data.resize(0);
}

int RefMap::findIdx(uint32_t key) {
    for (unsigned i = 0; i < data.size(); ++i) {
        if (data[i].key >> 1 == key)
            return i;
    }
    return -1;
}

void RefMap::print() {
    DMESG("RefMap %p r=%d size=%d", this, refcnt, data.size());
}

#ifdef PXT_MEMLEAK_DEBUG
std::set<TValue> allptrs;
void debugMemLeaks() {
    DMESG("LIVE POINTERS:");
    for (std::set<TValue>::iterator itr = allptrs.begin(); itr != allptrs.end(); itr++) {
        anyPrint(*itr);
    }
    DMESG("LIVE POINTERS END.");
    dumpDmesg();
}
#else
void debugMemLeaks() {}
#endif

CodalUSB usb;
HF2 hf2;

// TODO extract these from uf2_info()?
static const char *string_descriptors[] = {
    "Example Corp.", "PXT Device", "42424242",
};

static void initCodal() {
    devTimer.init();

    // Bring up fiber scheduler.
    scheduler_init(devMessageBus);

    // Seed our random number generator
    // seedRandom();

    // Create an event handler to trap any handlers being created for I2C services.
    // We do this to enable initialisation of those services only when they're used,
    // which saves processor time, memeory and battery life.
    // messageBus.listen(MICROBIT_ID_MESSAGE_BUS_LISTENER, MICROBIT_EVT_ANY, this,
    // &MicroBit::onListenerRegisteredEvent);

    io = new DevPins();

    usb.stringDescriptors = string_descriptors;
    usb.add(hf2);
    usb.start();
}

// ---------------------------------------------------------------------------
// An adapter for the API expected by the run-time.
// ---------------------------------------------------------------------------

map<pair<int, int>, Action> handlersMap;

DeviceEvent lastEvent;
DeviceTimer devTimer;
DeviceMessageBus devMessageBus;

// We have the invariant that if [dispatchEvent] is registered against the DAL
// for a given event, then [handlersMap] contains a valid entry for that
// event.
void dispatchEvent(DeviceEvent e) {
    lastEvent = e;

    Action curr = handlersMap[{e.source, e.value}];
    if (curr)
        runAction1(curr, fromInt(e.value));

    curr = handlersMap[{e.source, DEVICE_EVT_ANY}];
    if (curr)
        runAction1(curr, fromInt(e.value));
}

void registerWithDal(int id, int event, Action a) {
    Action prev = handlersMap[{id, event}];
    if (prev)
        decr(prev);
    else
        devMessageBus.listen(id, event, dispatchEvent);
    incr(a);
    handlersMap[{id, event}] = a;
}

void fiberDone(void *a) {
    decr((Action)a);
    release_fiber();
}

void runInBackground(Action a) {
    if (a != 0) {
        incr(a);
        create_fiber((void (*)(void *))runAction0, (void *)a, fiberDone);
    }
}

void waitForEvent(int id, int event)
{
    fiber_wait_for_event(id, event);
}


void error(ERROR code, int subcode) {
    DMESG("Error: %d [%d]", code, subcode);
    device.panic(42);
}

uint16_t *bytecode;
TValue *globals;
int numGlobals;

uint32_t *allocate(uint16_t sz) {
    uint32_t *arr = new uint32_t[sz];
    memset(arr, 0, sz * 4);
    return arr;
}

void checkStr(bool cond, const char *msg) {
    if (!cond) {
        while (true) {
            // uBit.display.scroll(msg, 100);
            // uBit.sleep(100);
        }
    }
}

int templateHash() {
    return ((int *)bytecode)[4];
}

int programHash() {
    return ((int *)bytecode)[6];
}

int getNumGlobals() {
    return bytecode[16];
}

void initRandomSeed() {
    int seed = 0xC0DA1;
    auto pinTemp = lookupPin(PIN_TEMPERATURE);
    if (pinTemp)
        seed *= pinTemp->getAnalogValue();
    auto pinLight = lookupPin(PIN_LIGHT);
    if (pinLight)
        seed *= pinLight->getAnalogValue();
    device.seedRandom(seed);
}

void clearNeoPixels() {
    // clear on-board neopixels
    auto neoPin = lookupPin(PIN_NEOPIXEL);
    if (neoPin) {
        uint8_t neobuf[30];
        memset(neobuf, 0, 30);
        neoPin->setDigitalValue(0);
        fiber_sleep(1);
        neopixel_send_buffer(*neoPin, neobuf, 30);
    }
}

void exec_binary(int32_t *pc) {
    // XXX re-enable once the calibration code is fixed and [editor/embedded.ts]
    // properly prepends a call to [internal_main].
    // ::touch_develop::internal_main();

    // unique group for radio based on source hash
    // ::touch_develop::micro_bit::radioDefaultGroup = programHash();

    // repeat error 4 times and restart as needed
    // microbit_panic_timeout(4);

    // TODO: fix this in CODAL
    device.seedRandom(0xC0DA1);

    int32_t ver = *pc++;
    checkStr(ver == 0x4209, ":( Bad runtime version");

    bytecode = *((uint16_t **)pc++); // the actual bytecode is here
    globals = (TValue *)allocate(getNumGlobals());

    // just compare the first word
    checkStr(((uint32_t *)bytecode)[0] == 0x923B8E70 && templateHash() == *pc,
             ":( Failed partial flash");

    uint32_t startptr = (uint32_t)bytecode;
    startptr += 48; // header
    startptr |= 1;  // Thumb state

    initCodal();
    initRandomSeed();
    clearNeoPixels();

    ((uint32_t(*)())startptr)();

#ifdef PXT_MEMLEAK_DEBUG
    pxt::debugMemLeaks();
#endif

    while (1) {
        fiber_sleep(10000);
    }
}

void start() {
    exec_binary((int32_t *)functionsAndBytecode);
}

} // end namespace

void RefCounted::destroy() {
#ifdef PXT_MEMLEAK_DEBUG
    allptrs.erase((TValue)this);
#endif
    free(this);
}

void RefCounted::init() {
    // Initialize to one reference (lowest bit set to 1)
    refCount = 3;
#ifdef PXT_MEMLEAK_DEBUG
    allptrs.insert((TValue)this);
#endif
}
