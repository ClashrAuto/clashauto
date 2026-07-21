/****************************************************************************
** Meta object code from reading C++ file 'AboutController.h'
**
** Created by: The Qt Meta Object Compiler version 68 (Qt 6.5.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../src/qml/AboutController.h"
#include <QtCore/qmetatype.h>

#if __has_include(<QtCore/qtmochelpers.h>)
#include <QtCore/qtmochelpers.h>
#else
QT_BEGIN_MOC_NAMESPACE
#endif


#include <memory>

#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'AboutController.h' doesn't include <QObject>."
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
struct qt_meta_stringdata_CLASSAboutControllerENDCLASS_t {};
static constexpr auto qt_meta_stringdata_CLASSAboutControllerENDCLASS = QtMocHelpers::stringData(
    "AboutController",
    "latestVersionChanged",
    "",
    "updateAvailableChanged",
    "checkingChanged",
    "checkFailed",
    "reason",
    "upToDate",
    "check",
    "currentVersion",
    "latestVersion",
    "updateAvailable",
    "checking",
    "releaseUrl"
);
#else  // !QT_MOC_HAS_STRING_DATA
struct qt_meta_stringdata_CLASSAboutControllerENDCLASS_t {
    uint offsetsAndSizes[28];
    char stringdata0[16];
    char stringdata1[21];
    char stringdata2[1];
    char stringdata3[23];
    char stringdata4[16];
    char stringdata5[12];
    char stringdata6[7];
    char stringdata7[9];
    char stringdata8[6];
    char stringdata9[15];
    char stringdata10[14];
    char stringdata11[16];
    char stringdata12[9];
    char stringdata13[11];
};
#define QT_MOC_LITERAL(ofs, len) \
    uint(sizeof(qt_meta_stringdata_CLASSAboutControllerENDCLASS_t::offsetsAndSizes) + ofs), len 
Q_CONSTINIT static const qt_meta_stringdata_CLASSAboutControllerENDCLASS_t qt_meta_stringdata_CLASSAboutControllerENDCLASS = {
    {
        QT_MOC_LITERAL(0, 15),  // "AboutController"
        QT_MOC_LITERAL(16, 20),  // "latestVersionChanged"
        QT_MOC_LITERAL(37, 0),  // ""
        QT_MOC_LITERAL(38, 22),  // "updateAvailableChanged"
        QT_MOC_LITERAL(61, 15),  // "checkingChanged"
        QT_MOC_LITERAL(77, 11),  // "checkFailed"
        QT_MOC_LITERAL(89, 6),  // "reason"
        QT_MOC_LITERAL(96, 8),  // "upToDate"
        QT_MOC_LITERAL(105, 5),  // "check"
        QT_MOC_LITERAL(111, 14),  // "currentVersion"
        QT_MOC_LITERAL(126, 13),  // "latestVersion"
        QT_MOC_LITERAL(140, 15),  // "updateAvailable"
        QT_MOC_LITERAL(156, 8),  // "checking"
        QT_MOC_LITERAL(165, 10)   // "releaseUrl"
    },
    "AboutController",
    "latestVersionChanged",
    "",
    "updateAvailableChanged",
    "checkingChanged",
    "checkFailed",
    "reason",
    "upToDate",
    "check",
    "currentVersion",
    "latestVersion",
    "updateAvailable",
    "checking",
    "releaseUrl"
};
#undef QT_MOC_LITERAL
#endif // !QT_MOC_HAS_STRING_DATA
} // unnamed namespace

Q_CONSTINIT static const uint qt_meta_data_CLASSAboutControllerENDCLASS[] = {

 // content:
      11,       // revision
       0,       // classname
       0,    0, // classinfo
       6,   14, // methods
       5,   58, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       5,       // signalCount

 // signals: name, argc, parameters, tag, flags, initial metatype offsets
       1,    0,   50,    2, 0x06,    6 /* Public */,
       3,    0,   51,    2, 0x06,    7 /* Public */,
       4,    0,   52,    2, 0x06,    8 /* Public */,
       5,    1,   53,    2, 0x06,    9 /* Public */,
       7,    0,   56,    2, 0x06,   11 /* Public */,

 // methods: name, argc, parameters, tag, flags, initial metatype offsets
       8,    0,   57,    2, 0x02,   12 /* Public */,

 // signals: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,    6,
    QMetaType::Void,

 // methods: parameters
    QMetaType::Void,

 // properties: name, type, flags
       9, QMetaType::QString, 0x00015401, uint(-1), 0,
      10, QMetaType::QString, 0x00015001, uint(0), 0,
      11, QMetaType::Bool, 0x00015001, uint(1), 0,
      12, QMetaType::Bool, 0x00015001, uint(2), 0,
      13, QMetaType::QString, 0x00015401, uint(-1), 0,

       0        // eod
};

Q_CONSTINIT const QMetaObject AboutController::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_meta_stringdata_CLASSAboutControllerENDCLASS.offsetsAndSizes,
    qt_meta_data_CLASSAboutControllerENDCLASS,
    qt_static_metacall,
    nullptr,
    qt_incomplete_metaTypeArray<qt_meta_stringdata_CLASSAboutControllerENDCLASS_t,
        // property 'currentVersion'
        QtPrivate::TypeAndForceComplete<QString, std::true_type>,
        // property 'latestVersion'
        QtPrivate::TypeAndForceComplete<QString, std::true_type>,
        // property 'updateAvailable'
        QtPrivate::TypeAndForceComplete<bool, std::true_type>,
        // property 'checking'
        QtPrivate::TypeAndForceComplete<bool, std::true_type>,
        // property 'releaseUrl'
        QtPrivate::TypeAndForceComplete<QString, std::true_type>,
        // Q_OBJECT / Q_GADGET
        QtPrivate::TypeAndForceComplete<AboutController, std::true_type>,
        // method 'latestVersionChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'updateAvailableChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'checkingChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'checkFailed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QString &, std::false_type>,
        // method 'upToDate'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'check'
        QtPrivate::TypeAndForceComplete<void, std::false_type>
    >,
    nullptr
} };

void AboutController::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<AboutController *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->latestVersionChanged(); break;
        case 1: _t->updateAvailableChanged(); break;
        case 2: _t->checkingChanged(); break;
        case 3: _t->checkFailed((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 4: _t->upToDate(); break;
        case 5: _t->check(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (AboutController::*)();
            if (_t _q_method = &AboutController::latestVersionChanged; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (AboutController::*)();
            if (_t _q_method = &AboutController::updateAvailableChanged; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (AboutController::*)();
            if (_t _q_method = &AboutController::checkingChanged; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (AboutController::*)(const QString & );
            if (_t _q_method = &AboutController::checkFailed; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (AboutController::*)();
            if (_t _q_method = &AboutController::upToDate; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 4;
                return;
            }
        }
    }else if (_c == QMetaObject::ReadProperty) {
        auto *_t = static_cast<AboutController *>(_o);
        (void)_t;
        void *_v = _a[0];
        switch (_id) {
        case 0: *reinterpret_cast< QString*>(_v) = _t->currentVersion(); break;
        case 1: *reinterpret_cast< QString*>(_v) = _t->latestVersion(); break;
        case 2: *reinterpret_cast< bool*>(_v) = _t->updateAvailable(); break;
        case 3: *reinterpret_cast< bool*>(_v) = _t->checking(); break;
        case 4: *reinterpret_cast< QString*>(_v) = _t->releaseUrl(); break;
        default: break;
        }
    } else if (_c == QMetaObject::WriteProperty) {
    } else if (_c == QMetaObject::ResetProperty) {
    } else if (_c == QMetaObject::BindableProperty) {
    }
}

const QMetaObject *AboutController::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *AboutController::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_CLASSAboutControllerENDCLASS.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int AboutController::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
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
    }else if (_c == QMetaObject::ReadProperty || _c == QMetaObject::WriteProperty
            || _c == QMetaObject::ResetProperty || _c == QMetaObject::BindableProperty
            || _c == QMetaObject::RegisterPropertyMetaType) {
        qt_static_metacall(this, _c, _id, _a);
        _id -= 5;
    }
    return _id;
}

// SIGNAL 0
void AboutController::latestVersionChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void AboutController::updateAvailableChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void AboutController::checkingChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 2, nullptr);
}

// SIGNAL 3
void AboutController::checkFailed(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}

// SIGNAL 4
void AboutController::upToDate()
{
    QMetaObject::activate(this, &staticMetaObject, 4, nullptr);
}
QT_WARNING_POP
