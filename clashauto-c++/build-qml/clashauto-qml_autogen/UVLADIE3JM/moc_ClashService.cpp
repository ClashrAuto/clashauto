/****************************************************************************
** Meta object code from reading C++ file 'ClashService.h'
**
** Created by: The Qt Meta Object Compiler version 68 (Qt 6.5.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../src/ClashService.h"
#include <QtNetwork/QSslError>
#include <QtCore/qmetatype.h>
#include <QtCore/QList>

#if __has_include(<QtCore/qtmochelpers.h>)
#include <QtCore/qtmochelpers.h>
#else
QT_BEGIN_MOC_NAMESPACE
#endif


#include <memory>

#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'ClashService.h' doesn't include <QObject>."
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
struct qt_meta_stringdata_CLASSClashServiceENDCLASS_t {};
static constexpr auto qt_meta_stringdata_CLASSClashServiceENDCLASS = QtMocHelpers::stringData(
    "ClashService",
    "trafficUpdated",
    "",
    "up",
    "down",
    "connectionsUpdated",
    "count",
    "downloadTotal",
    "nodesUpdated",
    "QList<NodeInfo>",
    "nodes",
    "selected",
    "proxyGroupsUpdated",
    "groups",
    "selectedGroup",
    "logUpdated",
    "message",
    "statusUpdated",
    "tun",
    "proxy",
    "core",
    "speedTestRunning",
    "running"
);
#else  // !QT_MOC_HAS_STRING_DATA
struct qt_meta_stringdata_CLASSClashServiceENDCLASS_t {
    uint offsetsAndSizes[46];
    char stringdata0[13];
    char stringdata1[15];
    char stringdata2[1];
    char stringdata3[3];
    char stringdata4[5];
    char stringdata5[19];
    char stringdata6[6];
    char stringdata7[14];
    char stringdata8[13];
    char stringdata9[16];
    char stringdata10[6];
    char stringdata11[9];
    char stringdata12[19];
    char stringdata13[7];
    char stringdata14[14];
    char stringdata15[11];
    char stringdata16[8];
    char stringdata17[14];
    char stringdata18[4];
    char stringdata19[6];
    char stringdata20[5];
    char stringdata21[17];
    char stringdata22[8];
};
#define QT_MOC_LITERAL(ofs, len) \
    uint(sizeof(qt_meta_stringdata_CLASSClashServiceENDCLASS_t::offsetsAndSizes) + ofs), len 
Q_CONSTINIT static const qt_meta_stringdata_CLASSClashServiceENDCLASS_t qt_meta_stringdata_CLASSClashServiceENDCLASS = {
    {
        QT_MOC_LITERAL(0, 12),  // "ClashService"
        QT_MOC_LITERAL(13, 14),  // "trafficUpdated"
        QT_MOC_LITERAL(28, 0),  // ""
        QT_MOC_LITERAL(29, 2),  // "up"
        QT_MOC_LITERAL(32, 4),  // "down"
        QT_MOC_LITERAL(37, 18),  // "connectionsUpdated"
        QT_MOC_LITERAL(56, 5),  // "count"
        QT_MOC_LITERAL(62, 13),  // "downloadTotal"
        QT_MOC_LITERAL(76, 12),  // "nodesUpdated"
        QT_MOC_LITERAL(89, 15),  // "QList<NodeInfo>"
        QT_MOC_LITERAL(105, 5),  // "nodes"
        QT_MOC_LITERAL(111, 8),  // "selected"
        QT_MOC_LITERAL(120, 18),  // "proxyGroupsUpdated"
        QT_MOC_LITERAL(139, 6),  // "groups"
        QT_MOC_LITERAL(146, 13),  // "selectedGroup"
        QT_MOC_LITERAL(160, 10),  // "logUpdated"
        QT_MOC_LITERAL(171, 7),  // "message"
        QT_MOC_LITERAL(179, 13),  // "statusUpdated"
        QT_MOC_LITERAL(193, 3),  // "tun"
        QT_MOC_LITERAL(197, 5),  // "proxy"
        QT_MOC_LITERAL(203, 4),  // "core"
        QT_MOC_LITERAL(208, 16),  // "speedTestRunning"
        QT_MOC_LITERAL(225, 7)   // "running"
    },
    "ClashService",
    "trafficUpdated",
    "",
    "up",
    "down",
    "connectionsUpdated",
    "count",
    "downloadTotal",
    "nodesUpdated",
    "QList<NodeInfo>",
    "nodes",
    "selected",
    "proxyGroupsUpdated",
    "groups",
    "selectedGroup",
    "logUpdated",
    "message",
    "statusUpdated",
    "tun",
    "proxy",
    "core",
    "speedTestRunning",
    "running"
};
#undef QT_MOC_LITERAL
#endif // !QT_MOC_HAS_STRING_DATA
} // unnamed namespace

Q_CONSTINIT static const uint qt_meta_data_CLASSClashServiceENDCLASS[] = {

 // content:
      11,       // revision
       0,       // classname
       0,    0, // classinfo
       7,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       7,       // signalCount

 // signals: name, argc, parameters, tag, flags, initial metatype offsets
       1,    2,   56,    2, 0x06,    1 /* Public */,
       5,    2,   61,    2, 0x06,    4 /* Public */,
       8,    2,   66,    2, 0x06,    7 /* Public */,
      12,    2,   71,    2, 0x06,   10 /* Public */,
      15,    1,   76,    2, 0x06,   13 /* Public */,
      17,    3,   79,    2, 0x06,   15 /* Public */,
      21,    1,   86,    2, 0x06,   19 /* Public */,

 // signals: parameters
    QMetaType::Void, QMetaType::LongLong, QMetaType::LongLong,    3,    4,
    QMetaType::Void, QMetaType::Int, QMetaType::LongLong,    6,    7,
    QMetaType::Void, 0x80000000 | 9, QMetaType::QString,   10,   11,
    QMetaType::Void, QMetaType::QStringList, QMetaType::QString,   13,   14,
    QMetaType::Void, QMetaType::QString,   16,
    QMetaType::Void, QMetaType::Bool, QMetaType::Bool, QMetaType::Bool,   18,   19,   20,
    QMetaType::Void, QMetaType::Bool,   22,

       0        // eod
};

Q_CONSTINIT const QMetaObject ClashService::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_meta_stringdata_CLASSClashServiceENDCLASS.offsetsAndSizes,
    qt_meta_data_CLASSClashServiceENDCLASS,
    qt_static_metacall,
    nullptr,
    qt_incomplete_metaTypeArray<qt_meta_stringdata_CLASSClashServiceENDCLASS_t,
        // Q_OBJECT / Q_GADGET
        QtPrivate::TypeAndForceComplete<ClashService, std::true_type>,
        // method 'trafficUpdated'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<qint64, std::false_type>,
        QtPrivate::TypeAndForceComplete<qint64, std::false_type>,
        // method 'connectionsUpdated'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<qint64, std::false_type>,
        // method 'nodesUpdated'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<QVector<NodeInfo>, std::false_type>,
        QtPrivate::TypeAndForceComplete<QString, std::false_type>,
        // method 'proxyGroupsUpdated'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<QStringList, std::false_type>,
        QtPrivate::TypeAndForceComplete<QString, std::false_type>,
        // method 'logUpdated'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<QString, std::false_type>,
        // method 'statusUpdated'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        // method 'speedTestRunning'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>
    >,
    nullptr
} };

void ClashService::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<ClashService *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->trafficUpdated((*reinterpret_cast< std::add_pointer_t<qint64>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<qint64>>(_a[2]))); break;
        case 1: _t->connectionsUpdated((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<qint64>>(_a[2]))); break;
        case 2: _t->nodesUpdated((*reinterpret_cast< std::add_pointer_t<QList<NodeInfo>>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QString>>(_a[2]))); break;
        case 3: _t->proxyGroupsUpdated((*reinterpret_cast< std::add_pointer_t<QStringList>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QString>>(_a[2]))); break;
        case 4: _t->logUpdated((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 5: _t->statusUpdated((*reinterpret_cast< std::add_pointer_t<bool>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<bool>>(_a[2])),(*reinterpret_cast< std::add_pointer_t<bool>>(_a[3]))); break;
        case 6: _t->speedTestRunning((*reinterpret_cast< std::add_pointer_t<bool>>(_a[1]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (ClashService::*)(qint64 , qint64 );
            if (_t _q_method = &ClashService::trafficUpdated; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (ClashService::*)(int , qint64 );
            if (_t _q_method = &ClashService::connectionsUpdated; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (ClashService::*)(QVector<NodeInfo> , QString );
            if (_t _q_method = &ClashService::nodesUpdated; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (ClashService::*)(QStringList , QString );
            if (_t _q_method = &ClashService::proxyGroupsUpdated; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (ClashService::*)(QString );
            if (_t _q_method = &ClashService::logUpdated; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 4;
                return;
            }
        }
        {
            using _t = void (ClashService::*)(bool , bool , bool );
            if (_t _q_method = &ClashService::statusUpdated; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 5;
                return;
            }
        }
        {
            using _t = void (ClashService::*)(bool );
            if (_t _q_method = &ClashService::speedTestRunning; *reinterpret_cast<_t *>(_a[1]) == _q_method) {
                *result = 6;
                return;
            }
        }
    }
}

const QMetaObject *ClashService::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *ClashService::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_CLASSClashServiceENDCLASS.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int ClashService::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 7)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 7;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 7)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 7;
    }
    return _id;
}

// SIGNAL 0
void ClashService::trafficUpdated(qint64 _t1, qint64 _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void ClashService::connectionsUpdated(int _t1, qint64 _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void ClashService::nodesUpdated(QVector<NodeInfo> _t1, QString _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void ClashService::proxyGroupsUpdated(QStringList _t1, QString _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}

// SIGNAL 4
void ClashService::logUpdated(QString _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 4, _a);
}

// SIGNAL 5
void ClashService::statusUpdated(bool _t1, bool _t2, bool _t3)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))) };
    QMetaObject::activate(this, &staticMetaObject, 5, _a);
}

// SIGNAL 6
void ClashService::speedTestRunning(bool _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 6, _a);
}
QT_WARNING_POP
