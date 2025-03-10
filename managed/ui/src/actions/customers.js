// Copyright (c) YugaByte, Inc.

import axios from 'axios';
import { ROOT_URL } from '../config';
import Cookies from 'js-cookie';

// Get current user(me) from token in localStorage
export const VALIDATE_FROM_TOKEN = 'VALIDATE_FROM_TOKEN';
export const VALIDATE_FROM_TOKEN_RESPONSE = 'VALIDATE_FROM_TOKEN_RESPONSE';

// Sign Up Customer
export const REGISTER = 'REGISTER';
export const REGISTER_RESPONSE = 'REGISTER_RESPONSE';

// Sign In Customer
export const LOGIN = 'LOGIN';
export const LOGIN_RESPONSE = 'LOGIN_RESPONSE';
export const INSECURE_LOGIN = 'INSECURE_LOGIN';
export const INSECURE_LOGIN_RESPONSE = 'INSECURE_LOGIN_RESPONSE';

export const RESET_CUSTOMER = 'RESET_CUSTOMER';

// log out Customer
export const LOGOUT = 'LOGOUT';
export const LOGOUT_SUCCESS = 'LOGOUT_SUCCESS';
export const LOGOUT_FAILURE = 'LOGOUT_FAILURE';

// Update Customer Profile
export const UPDATE_PROFILE = 'UPDATE_PROFILE';
export const UPDATE_PROFILE_SUCCESS = 'UPDATE_PROFILE_SUCCESS';
export const UPDATE_PROFILE_FAILURE = 'UPDATE_PROFILE_FAILURE';

// Fetch Software Versions for Customer
export const FETCH_SOFTWARE_VERSIONS = 'FETCH_SOFTWARE_VERSIONS';
export const FETCH_SOFTWARE_VERSIONS_SUCCESS = 'FETCH_SOFTWARE_VERSIONS_SUCCESS';
export const FETCH_SOFTWARE_VERSIONS_FAILURE = 'FETCH_SOFTWARE_VERSIONS_FAILURE';

export const FETCH_HOST_INFO = 'FETCH_HOST_INFO';
export const FETCH_HOST_INFO_SUCCESS = 'FETCH_HOST_INFO_SUCCESS';
export const FETCH_HOST_INFO_FAILURE = 'FETCH_HOST_INFO_FAILURE';

export const FETCH_CUSTOMER_COUNT = 'FETCH_CUSTOMER_COUNT';

export const FETCH_YUGAWARE_VERSION = 'FETCH_YUGAWARE_VERSION';
export const FETCH_YUGAWARE_VERSION_RESPONSE = 'FETCH_YUGAWARE_VERSION_RESPONSE';

export const ADD_CUSTOMER_CONFIG = 'ADD_CUSTOMER_CONFIG';
export const ADD_CUSTOMER_CONFIG_RESPONSE = 'ADD_CUSTOMER_CONFIG_RESPONSE';

export const DELETE_CUSTOMER_CONFIG = 'DELETE_CUSTOMER_CONFIG';
export const DELETE_CUSTOMER_CONFIG_RESPONSE = 'DELETE_CUSTOMER_CONFIG_RESPONSE';

export const FETCH_CUSTOMER_CONFIGS = 'FETCH_CUSTOMER_CONFIGS';
export const FETCH_CUSTOMER_CONFIGS_RESPONSE = 'FETCH_CUSTOMER_CONFIGS_RESPONSE';

export const INVALID_CUSTOMER_TOKEN = 'INVALID_CUSTOMER_TOKEN';
export const RESET_TOKEN_ERROR = 'RESET_TOKEN_ERROR';

export const GET_LOGS = 'GET_LOGS';
export const GET_LOGS_SUCCESS = 'GET_LOGS_SUCCESS';
export const GET_LOGS_FAILURE = 'GET_LOGS_FAILURE';

export const GET_RELEASES = 'GET_RELEASES';
export const GET_RELEASES_RESPONSE = 'GET_RELEASES_RESPONSE';

export const FETCH_TLS_CERTS = 'FETCH_TLS_CERTS';
export const FETCH_TLS_CERTS_RESPONSE = 'FETCH_TLS_CERTS_RESPONSE';

export const REFRESH_RELEASES = 'REFRESH_RELEASES';
export const REFRESH_RELEASES_RESPONSE = 'REFRESH_RELEASES_RESPONSE';

export const IMPORT_RELEASE = 'IMPORT_RELEASE';
export const IMPORT_RELEASE_RESPONSE = 'IMPORT_RELEASE_RESPONSE';

export const UPDATE_RELEASE = 'UPDATE_RELEASE';
export const UPDATE_RELEASE_RESPONSE = 'UPDATE_RELEASE_RESPONSE';

export function validateToken() {
  let cUUID = Cookies.get("customerId");
  if (cUUID) {
    localStorage.setItem("customerId", cUUID);
  } else {
    cUUID = localStorage.getItem("customerId");
  }
  const authToken = Cookies.get("authToken") || localStorage.getItem("authToken");
  axios.defaults.headers.common['X-AUTH-TOKEN'] = authToken;
  const apiToken = Cookies.get("apiToken") || localStorage.getItem("apiToken");
  if (apiToken && apiToken !== '') {
    axios.defaults.headers.common['X-AUTH-YW-API-TOKEN'] = apiToken;
  }
  const request = axios(`${ROOT_URL}/customers/${cUUID}`);
  return {
    type: VALIDATE_FROM_TOKEN,
    payload: request
  };
}

export function validateFromTokenResponse(response) {
  return {
    type: VALIDATE_FROM_TOKEN_RESPONSE,
    payload: response
  };
}

export function register(formValues) {
  const request = axios.post(`${ROOT_URL}/register`, formValues);
  return {
    type: REGISTER,
    payload: request
  };
}

export function registerResponse(response) {
  return {
    type: REGISTER_RESPONSE,
    payload: response
  };
}

export function login(formValues) {
  const request = axios.post(`${ROOT_URL}/login`, formValues);
  return {
    type: LOGIN,
    payload: request
  };
}

export function loginResponse(response) {
  return {
    type: LOGIN_RESPONSE,
    payload: response
  };
}

export function insecureLogin() {
  const request = axios.get(`${ROOT_URL}/insecure_login`);
  return {
    type: INSECURE_LOGIN,
    payload: request
  };
}

export function insecureLoginResponse(response) {
  return {
    type: INSECURE_LOGIN_RESPONSE,
    payload: response
  };
}

export function logout() {
  const request = axios.get(`${ROOT_URL}/logout`);
  return {
    type: LOGOUT,
    payload: request
  };
}

export function logoutSuccess() {
  return {
    type: LOGOUT_SUCCESS
  };
}

export function logoutFailure(error) {
  return {
    type: LOGOUT_FAILURE,
    payload: error
  };
}

export function resetCustomer() {
  return {
    type: RESET_CUSTOMER
  };
}

export function customerTokenError() {
  return {
    type: INVALID_CUSTOMER_TOKEN
  };
}

export function resetCustomerError() {
  return {
    type: RESET_TOKEN_ERROR
  };
}

export function updateProfile(values) {
  const cUUID = localStorage.getItem("customerId");
  const request = axios.put(`${ROOT_URL}/customers/${cUUID}`, values);
  return {
    type: UPDATE_PROFILE,
    payload: request
  };
}

export function updateProfileSuccess(response) {
  return {
    type: UPDATE_PROFILE_SUCCESS,
    payload: response
  };
}

export function updateProfileFailure(error) {
  return {
    type: UPDATE_PROFILE_FAILURE,
    payload: error
  };
}

export function fetchSoftwareVersions() {
  const cUUID = localStorage.getItem("customerId");
  const request = axios.get(`${ROOT_URL}/customers/${cUUID}/releases`);
  return {
    type: FETCH_SOFTWARE_VERSIONS,
    payload: request
  };
}

export function fetchSoftwareVersionsSuccess(result) {
  return {
    type: FETCH_SOFTWARE_VERSIONS_SUCCESS,
    payload: result
  };
}

export function fetchSoftwareVersionsFailure(error) {
  return {
    type: FETCH_SOFTWARE_VERSIONS_FAILURE,
    payload: error
  };
}

export function getTlsCertificates() {
  const cUUID = localStorage.getItem('customerId');
  const request = axios.get(`${ROOT_URL}/customers/${cUUID}/certificates`);
  return {
    type: FETCH_TLS_CERTS,
    payload: request
  };
}

export function getTlsCertificatesResponse(result) {
  return {
    type: FETCH_TLS_CERTS_RESPONSE,
    payload: result
  };
}

export function fetchHostInfo() {
  const cUUID = localStorage.getItem("customerId");
  const request = axios.get(`${ROOT_URL}/customers/${cUUID}/host_info`);
  return {
    type: FETCH_HOST_INFO,
    payload: request
  };
}

export function fetchHostInfoSuccess(result) {
  return {
    type: FETCH_HOST_INFO_SUCCESS,
    payload: result
  };
}

export function fetchHostInfoFailure(error) {
  return {
    type: FETCH_HOST_INFO_FAILURE,
    payload: error
  };
}

export function fetchCustomerCount() {
  const request = axios.get(`${ROOT_URL}/customer_count`);
  return {
    type: FETCH_CUSTOMER_COUNT,
    payload: request
  };
}

export function fetchYugaWareVersion() {
  const request = axios.get(`${ROOT_URL}/app_version`);
  return {
    type: FETCH_YUGAWARE_VERSION,
    payload: request
  };
}

export function fetchYugaWareVersionResponse(response) {
  return {
    type: FETCH_YUGAWARE_VERSION_RESPONSE,
    payload: response
  };
}

export function addCustomerConfig(config) {
  const cUUID = localStorage.getItem("customerId");
  const request = axios.post(`${ROOT_URL}/customers/${cUUID}/configs`, config);
  return {
    type: ADD_CUSTOMER_CONFIG,
    payload: request
  };
}

export function addCustomerConfigResponse(response) {
  return {
    type: ADD_CUSTOMER_CONFIG_RESPONSE,
    payload: response
  };
}

export function deleteCustomerConfig(configUUID) {
  const cUUID = localStorage.getItem("customerId");
  const request = axios.delete(`${ROOT_URL}/customers/${cUUID}/configs/${configUUID}`);
  return {
    type: DELETE_CUSTOMER_CONFIG,
    payload: request
  };
}

export function deleteCustomerConfigResponse(response) {
  return {
    type: DELETE_CUSTOMER_CONFIG_RESPONSE,
    payload: response
  };
}

export function fetchCustomerConfigs() {
  const cUUID = localStorage.getItem("customerId");
  const request = axios.get(`${ROOT_URL}/customers/${cUUID}/configs`);
  return {
    type: FETCH_CUSTOMER_CONFIGS,
    payload: request
  };
}

export function fetchCustomerConfigsResponse(response) {
  return {
    type: FETCH_CUSTOMER_CONFIGS_RESPONSE,
    payload: response
  };
}

export function getLogs() {
  // TODO(bogdan): Maybe make this a URL param somehow?
  const request = axios.get(`${ROOT_URL}/logs/1000`);
  return {
    type: FETCH_HOST_INFO,
    payload: request
  };
}

export function getLogsSuccess(result) {
  return {
    type: GET_LOGS_SUCCESS,
    payload: result
  };
}

export function getLogsFailure(error) {
  return {
    type: GET_LOGS_FAILURE,
    payload: error
  };
}

export function getYugaByteReleases() {
  const cUUID = localStorage.getItem("customerId");
  const request = axios.get(`${ROOT_URL}/customers/${cUUID}/releases?includeMetadata=true`);
  return {
    type: GET_RELEASES,
    payload: request
  };
}

export function getYugaByteReleasesResponse(response) {
  return {
    type: GET_RELEASES_RESPONSE,
    payload: response
  };
}

export function refreshYugaByteReleases() {
  const cUUID = localStorage.getItem("customerId");
  const request = axios.put(`${ROOT_URL}/customers/${cUUID}/releases`);
  return {
    type: REFRESH_RELEASES,
    payload: request
  };
}

export function refreshYugaByteReleasesResponse(response) {
  return {
    type: REFRESH_RELEASES_RESPONSE,
    payload: response
  };
}

export function importYugaByteRelease(payload) {
  const cUUID = localStorage.getItem("customerId");
  const request = axios.post(`${ROOT_URL}/customers/${cUUID}/releases`, payload);
  return {
    type: IMPORT_RELEASE,
    payload: request
  };
}

export function importYugaByteReleaseResponse(response) {
  return {
    type: IMPORT_RELEASE_RESPONSE,
    payload: response
  };
}

export function updateYugaByteRelease(version, payload) {
  const cUUID = localStorage.getItem("customerId");
  const request = axios.put(`${ROOT_URL}/customers/${cUUID}/releases/${version}`, payload);
  return {
    type: UPDATE_RELEASE,
    payload: request
  };
}

export function updateYugaByteReleaseResponse(response) {
  return {
    type: UPDATE_RELEASE_RESPONSE,
    payload: response
  };
}
