#ifndef GRAPHIC_VECTOR4D
#define GRAPHIC_VECTOR4D

#include <stdint.h>

class Vector4D;

#include <vec3d.h>

class Vector4D
{
public:
	Vector4D(void) {vec[0] = 0; vec[1] = 0; vec[2] = 0; vec[3] = 1;}
	Vector4D(float x, float y, float z, float w);
	Vector4D(const Vector3D& v, float w = 1.f);
	inline float x(void) const {return vec[0];}
	inline float y(void) const {return vec[1];}
	inline float z(void) const {return vec[2];}
	inline float w(void) const {return vec[3];}
	inline void setX(float x) {vec[0] = x;}
	inline void setY(float y) {vec[1] = y;}
	inline void setZ(float z) {vec[2] = z;}
	inline void setW(float w) {vec[3] = w;}
	inline uint32_t toColour(void) const;

	float& operator[](int32_t i) {return vec[i];}
	const float& operator[](int32_t i) const {return vec[i];}
	Vector4D& operator+=(const Vector4D& vector);
	Vector4D& operator-=(const Vector4D& vector);
	Vector4D& operator*=(const Vector4D& vector);
	Vector4D& operator/=(const Vector4D& vector);
	Vector4D& operator*=(float m);
	Vector4D& operator/=(float d);

private:
	float vec[4];
};

const Vector4D operator+(const Vector4D& v1, const Vector4D& v2);
const Vector4D operator-(const Vector4D& v1, const Vector4D& v2);
const Vector4D operator*(const Vector4D& v1, const Vector4D& v2);
const Vector4D operator/(const Vector4D& v1, const Vector4D& v2);
const Vector4D operator-(const Vector4D& vector);
const Vector4D operator*(const Vector4D& v1, float m);
static inline const Vector4D operator*(float m, const Vector4D& v1) {return v1 * m;}
const Vector4D operator/(const Vector4D& v1, float d);

// inline functions

inline uint32_t Vector4D::toColour(void) const
{
	uint32_t a = 255 * w(), r = 255 * x(), g = 255 * y(), b = 255 * z();
	return ((a & 0xFF) << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
}

#endif