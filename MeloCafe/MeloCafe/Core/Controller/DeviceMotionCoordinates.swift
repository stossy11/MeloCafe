// Core Motion reports a downward gravity vector in the device's portrait axes.
// Rotate into display axes first, then into the fusion filter's +Y-up frame.
// Gyro and upward acceleration must use the same proper rotation.
enum DeviceMotionCoordinates {
    static func display(_ value: SIMD3<Float>, quarterTurns: Int) -> SIMD3<Float> {
        switch quarterTurns {
        case 1: return SIMD3(-value.y, value.x, value.z)
        case -1: return SIMD3(value.y, -value.x, value.z)
        case 2: return SIMD3(-value.x, -value.y, value.z)
        default: return value
        }
    }

    static func convert(gyro: SIMD3<Float>, acceleration: SIMD3<Float>, quarterTurns: Int)
        -> (gyro: SIMD3<Float>, acceleration: SIMD3<Float>) {
        let g = display(gyro, quarterTurns: quarterTurns)
        let a = display(acceleration, quarterTurns: quarterTurns)
        return (SIMD3(g.x, g.z, -g.y), SIMD3(-a.x, -a.z, a.y))
    }
}
