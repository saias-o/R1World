#pragma once
#include <cstddef>
namespace r1::traffic_format {
constexpr int kSchemaVersion=3;
enum LaneField : size_t {
    From,To,Speed,Weight,Offset,Width,Road,Number,Priority,Signal,Group,Stop,Yield,AllowedTurns,Layer,StopDistance,FieldCount
};
}
