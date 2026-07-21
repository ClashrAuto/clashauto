/****************************************************************************
** Meta object code from reading C++ file 'CoreController.h'
**
** Created by: The Qt Meta Object Compiler version 68 (Qt 6.5.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../src/CoreController.h"
#include <QtNetwork/QSslError>
#include <QtCore/qmetatype.h>

#if __has_include(<QtCore/qtmochelpers.h>)
#include <QtCore/qtmochelpers.h>
#else
QT_BEGIN_MOC_NAMESPACE
#endif


#include <memory>

#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'CoreController.h' doesn't include <QObject>."
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
struct qt_meta_stringdata_CLASSCoreControllerENDCLASS_t {};
static constexpr auto qt_meta_stringdata_CLASSCoreControllerENDCLASS = QtMocHelpers::stringData(
    "CoreController",
    "statusChanged",
    "",
    "tun",
    "proxy",
    "core",
    "logUpdated",
    "message",
    "coreMissing",
    "path",
    "startCore",
    "stopCore",
    "toggleCore",
    "toggleProxy",
    "toggleTun",
    "rebuildConfig"
);
#else  // !QT_MOC_HAS_STRING_DATA
struct qt_meta_stringdata_CLASSCoreControllerENDCLASS_t {
    uint offsetsAndSizes[32];
    char stringdata0[15];
    char stringdata1[14];
    char stringdata2[1];
    char stringdata3[4];
    char stringdata4[6];
    char stringdata5[5];
    char stringdata6[11];
    char stringdata7[8];
    char stringdata8[12];
    char stringdata9[5];
    char stringdata10[10];
    char stringdata11[9];
    char stringdata12[11];
    char stringdata13[12];
    char stringdata14[10];
    char stringdata15[14];
};
#define QT_MOC_LITERAL(ofs, len) \
    uint(sizeof(qt_meta_stringdata_CLASSCoreControllerENDCLASS_t::offsetsAndSizes) + ofs), len 
Q_CONSTINIT static const qt_meta_stringdata_CLASSCoreControllerENDCLASS_t qt_meta_stringdata_CLASSCoreControllerENDCLASS = {
    {
        QT_MOC_LITERAL(0, 14),  // "CoreController"
        QT_MOC_LITERAL(15, 13),  // "statusChanged"
        QT_MOC_LITERAL(29, 0),  // ""
        QT_MOC_LITERAL(30, 3),  // "tun"
        QT_MOC_LITERAL(34, 5),  // "proxy"
        QT_MOC_LITERAL(40, 4),  // "core"
        QT_MOC_LITERAL(45, 10),  // "logUpdated"
        QT_MOC_LITERAL(56, 7),  // "message"
        QT_MOC_LITERAL(64, 11),  // "coreMissing"
        QT_MOC_LITERAL(76, 4),  // "path"
        QT_MOC_LITERAL(81, 9),  // "startCore"
        QT_MOC_LITERAL(91, 8),  // "stopCore"
        QT_MOC_LITERAL(100, 10),  // "toggleCore"
        QT_MOC_LITERAL(111, 11),  // "toggleProxy"
        QT_MOC_LITERAL(123, 9),  // "toggleTun"
        QT_MOC_LITERAL(133, 13)   // "rebuildConfig"
    },
    "CoreController",
    "statusChanged",
    "",
    "tun",
    "proxy",
    "core",
    "logUpdated",
    "message",
    "coreMissing",
    "path",
    "startCore",
    "stopCore",
    "toggleCore",
    "toggleProxy",
    "toggleTun",
    "rebuildConfig"
};
#undef QT_MOC_LITERAL
#endif // !QT_MOC_HAS_STRING_DATA
} // unnamed namespace

Q_CONSTINIT static const uint qt_meta_data_CLASSCoreControllerENDCLASS[] = {

 // content:
      11,       // revision
       0,       // classname
       0,    0, // classinfo
       9,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       3,       // signalCount

 // signals: name, argc, parameters, tag, flags, initial metatype offsets
       1,    3,   68,    2, 0x06,    1 /* Public */,
       6,    1,   75,    2, 0x06,    5 /* Public */,
       8,    1,   78,    2, 0x06,    7 /* Public */,

 // slots: name, argc, parameters, tag, flags, initial metatype offsets
      10,    0,   81,    2, 0x0a,    9 /* Public */,
      11,    0,   82,    2, 0x0a,   10 /* Public */,
      12,    0,   83,    2, 0x0a,   11 /* Public */,
      13,    0,   84,    2, 0x0a,   12 /* Public */,
      14,    0,   85,    2, 0x0a,   13 /* Public */,
      15,    0,   86,    2, 0x0a,   14 /* Public */,

 // signals: parameters
    QMetaType::Void, QMetaType::Bool, QMetaType::Bool, QMetaType::Bool,    3,    4,    5,
    QMetaType::Void, QMetaType::QString,    7,
    QMetaType::Void, QMetaType::QString,    9,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,

       0        // eod
};

Q_CONSTINIT const QMetaObject CoreController::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_meta_stringdata_CLASSCoreControllerENDCLASS.offsetsAndSizes,
    qt_meta_data_CLASSCoreControllerENDCLASS,
    qt_static_metacall,
    nullptr,
    qt_incomplete_metaTypeArray<qt_meta_stringdata_CLASSCoreControllerENDCLASS_t,
        // Q_OBJECT / Q_GADGET
        QtPrivate::TypeAndForceComplete<CoreController, std::true_type>,
        // method 'statusChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        // method 'logUpdated'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<QString, std::false_type>,
        // method 'coreMissing'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<QString, std::false_type>,
        // method 'startCore'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'stopCore'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'toggleCore'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'toggleProxy'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'toggleTun'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'rebuildConfig'
        QtPrivate::TypeAndForceComplete<void, std::false_type>
    >,
    nullptr
} };

void CoreController::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<CoreController *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->statusChanged((*reinterpret_cast< std::add_pointer_t<bool>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<bool>>(_a[2])),(*reinterpret_cast< std::add_pointer_t<bool>>(_a[3]))); break;
        case 1: _t->logUpdated((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 2: _t->coreMissing((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 3: _t->startCore(); break;
        case 4: _t->stopCore(); break;
        case 5: _t->toggleCore(); break;
        case 6: _t->toggleProxy(); break;
        case 7: _t->toggleTun(); break;
        case 8: _t->rebuildConfig(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (CoreController::*)(bool , bool , bool );
            if (_t _q_method = &CoreController::statusChanged; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (CoreController::*)(QString );
            if (_t _q_method = &CoreController::logUpdated; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (CoreController::*)(QString );
            if (_t _q_method = &CoreController::coreMissing; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 2;
                return;
            }
        }
    }
}

const QMetaObject *CoreController::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *CoreController::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_CLASSCoreControllerENDCLASS.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int CoreController::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 9)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 9;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 9)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 9;
    }
    return _id;
}

// SIGNAL 0
void CoreController::statusChanged(bool _t1, bool _t2, bool _t3)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void CoreController::logUpdated(QString _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void CoreController::coreMissing(QString _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}
QT_WARNING_POP
