/****************************************************************************
** Meta object code from reading C++ file 'TrayController.h'
**
** Created by: The Qt Meta Object Compiler version 68 (Qt 6.5.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../src/TrayController.h"
#include <QtCore/qmetatype.h>

#if __has_include(<QtCore/qtmochelpers.h>)
#include <QtCore/qtmochelpers.h>
#else
QT_BEGIN_MOC_NAMESPACE
#endif


#include <memory>

#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'TrayController.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 68
#error "This file was generated using the moc from 6.5.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {

#ifdef QT_MOC_HAS_STRINGDATA
struct qt_meta_stringdata_CLASSTrayControllerENDCLASS_t {};
static constexpr auto qt_meta_stringdata_CLASSTrayControllerENDCLASS = QtMocHelpers::stringData(
    "TrayController",
    "toggleCoreRequested",
    "",
    "toggleProxyRequested",
    "toggleTunRequested",
    "setStatus",
    "tun",
    "proxy",
    "core",
    "setTraffic",
    "up",
    "down",
    "notify",
    "title",
    "message"
);
#else  // !QT_MOC_HAS_STRING_DATA
struct qt_meta_stringdata_CLASSTrayControllerENDCLASS_t {
    uint offsetsAndSizes[30];
    char stringdata0[15];
    char stringdata1[20];
    char stringdata2[1];
    char stringdata3[21];
    char stringdata4[19];
    char stringdata5[10];
    char stringdata6[4];
    char stringdata7[6];
    char stringdata8[5];
    char stringdata9[11];
    char stringdata10[3];
    char stringdata11[5];
    char stringdata12[7];
    char stringdata13[6];
    char stringdata14[8];
};
#define QT_MOC_LITERAL(ofs, len) \
    uint(sizeof(qt_meta_stringdata_CLASSTrayControllerENDCLASS_t::offsetsAndSizes) + ofs), len 
Q_CONSTINIT static const qt_meta_stringdata_CLASSTrayControllerENDCLASS_t qt_meta_stringdata_CLASSTrayControllerENDCLASS = {
    {
        QT_MOC_LITERAL(0, 14),  // "TrayController"
        QT_MOC_LITERAL(15, 19),  // "toggleCoreRequested"
        QT_MOC_LITERAL(35, 0),  // ""
        QT_MOC_LITERAL(36, 20),  // "toggleProxyRequested"
        QT_MOC_LITERAL(57, 18),  // "toggleTunRequested"
        QT_MOC_LITERAL(76, 9),  // "setStatus"
        QT_MOC_LITERAL(86, 3),  // "tun"
        QT_MOC_LITERAL(90, 5),  // "proxy"
        QT_MOC_LITERAL(96, 4),  // "core"
        QT_MOC_LITERAL(101, 10),  // "setTraffic"
        QT_MOC_LITERAL(112, 2),  // "up"
        QT_MOC_LITERAL(115, 4),  // "down"
        QT_MOC_LITERAL(120, 6),  // "notify"
        QT_MOC_LITERAL(127, 5),  // "title"
        QT_MOC_LITERAL(133, 7)   // "message"
    },
    "TrayController",
    "toggleCoreRequested",
    "",
    "toggleProxyRequested",
    "toggleTunRequested",
    "setStatus",
    "tun",
    "proxy",
    "core",
    "setTraffic",
    "up",
    "down",
    "notify",
    "title",
    "message"
};
#undef QT_MOC_LITERAL
#endif // !QT_MOC_HAS_STRING_DATA
} // unnamed namespace

Q_CONSTINIT static const uint qt_meta_data_CLASSTrayControllerENDCLASS[] = {

 // content:
      11,       // revision
       0,       // classname
       0,    0, // classinfo
       6,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       3,       // signalCount

 // signals: name, argc, parameters, tag, flags, initial metatype offsets
       1,    0,   50,    2, 0x06,    1 /* Public */,
       3,    0,   51,    2, 0x06,    2 /* Public */,
       4,    0,   52,    2, 0x06,    3 /* Public */,

 // slots: name, argc, parameters, tag, flags, initial metatype offsets
       5,    3,   53,    2, 0x0a,    4 /* Public */,
       9,    2,   60,    2, 0x0a,    8 /* Public */,
      12,    2,   65,    2, 0x0a,   11 /* Public */,

 // signals: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,

 // slots: parameters
    QMetaType::Void, QMetaType::Bool, QMetaType::Bool, QMetaType::Bool,    6,    7,    8,
    QMetaType::Void, QMetaType::LongLong, QMetaType::LongLong,   10,   11,
    QMetaType::Void, QMetaType::QString, QMetaType::QString,   13,   14,

       0        // eod
};

Q_CONSTINIT const QMetaObject TrayController::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_meta_stringdata_CLASSTrayControllerENDCLASS.offsetsAndSizes,
    qt_meta_data_CLASSTrayControllerENDCLASS,
    qt_static_metacall,
    nullptr,
    qt_incomplete_metaTypeArray<qt_meta_stringdata_CLASSTrayControllerENDCLASS_t,
        // Q_OBJECT / Q_GADGET
        QtPrivate::TypeAndForceComplete<TrayController, std::true_type>,
        // method 'toggleCoreRequested'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'toggleProxyRequested'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'toggleTunRequested'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'setStatus'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        // method 'setTraffic'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<qint64, std::false_type>,
        QtPrivate::TypeAndForceComplete<qint64, std::false_type>,
        // method 'notify'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QString &, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QString &, std::false_type>
    >,
    nullptr
} };

void TrayController::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<TrayController *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->toggleCoreRequested(); break;
        case 1: _t->toggleProxyRequested(); break;
        case 2: _t->toggleTunRequested(); break;
        case 3: _t->setStatus((*reinterpret_cast< std::add_pointer_t<bool>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<bool>>(_a[2])),(*reinterpret_cast< std::add_pointer_t<bool>>(_a[3]))); break;
        case 4: _t->setTraffic((*reinterpret_cast< std::add_pointer_t<qint64>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<qint64>>(_a[2]))); break;
        case 5: _t->notify((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QString>>(_a[2]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (TrayController::*)();
            if (_t _q_method = &TrayController::toggleCoreRequested; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (TrayController::*)();
            if (_t _q_method = &TrayController::toggleProxyRequested; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (TrayController::*)();
            if (_t _q_method = &TrayController::toggleTunRequested; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 2;
                return;
            }
        }
    }
}

const QMetaObject *TrayController::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *TrayController::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_CLASSTrayControllerENDCLASS.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int TrayController::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 6)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 6;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 6)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 6;
    }
    return _id;
}

// SIGNAL 0
void TrayController::toggleCoreRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void TrayController::toggleProxyRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void TrayController::toggleTunRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 2, nullptr);
}
QT_WARNING_POP
