#ifndef GRAPHIC_VECTOR3D
#define GRAPHIC_VECTOR3D

#include <stdint.h>

class Vector3D;

#include <vec4d.h>

class Vector3D
{
public:
	Vector3D(void) {vec[0] = 0; vec[1] = 0; vec[2] = 0;}
	Vector3D(float x, float y, float z);
	Vector3D(const Vector4D& v);
	inline float x(void) const {return vec[0];}
	inline float y(void) const {return vec[1];}
	inline float z(void) const {return vec[2];}
	inline void setX(float x) {vec[0] = x;}
	inline void setY(float y) {vec[1] = y;}
	inline void setZ(float z) {vec[2] = z;}
	inline uint32_t toColour(void) const;
	float length() const;
	inline Vector3D normalized(void) const;
	static Vector3D crossProduct(const Vector3D& v1, const Vector3D& v2);

	float& operator[](int32_t i) {return vec[i];}
	const float& operator[](int32_t i) const {return vec[i];}
	Vector3D& operator+=(const Vector3D& vector);
	Vector3D& operator-=(const Vector3D& vector);
	Vector3D& operator*=(const Vector3D& vector);
	Vector3D& operator/=(const Vector3D& vector);
	Vector3D& operator*=(float m);
	Vector3D& operator/=(float d);

private:
	float vec[3];
};

const Vector3D operator+(const Vector3D& v1, const Vector3D& v2);
const Vector3D operator-(const Vector3D& v1, const Vector3D& v2);
const Vector3D operator*(const Vector3D& v1, const Vector3D& v2);
const Vector3D operator/(const Vector3D& v1, const Vector3D& v2);
const Vector3D operator-(const Vector3D& vector);
const Vector3D operator*(const Vector3D& v1, float m);
static inline const Vector3D operator*(float m, const Vector3D& v1) {return v1 * m;}
const Vector3D operator/(const Vector3D& v1, float d);

// inline functions

inline uint32_t Vector3D::toColour(void) const
{
	uint32_t r = 255 * x(), g = 255 * y(), b = 255 * z();
	return ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
}

inline Vector3D Vector3D::normalized(void) const
{
	return (*this) / length();
}

#endif